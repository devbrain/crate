#pragma once

#include <array>
#include <crate/core/decompressor.hh>
#include <crate/core/types.hh>

namespace crate {

// Quantum constants
namespace quantum {
// Valid window size range (in bits)
constexpr unsigned MIN_WINDOW_BITS = 10;  // 1KB minimum
constexpr unsigned MAX_WINDOW_BITS = 21;  // 2MB maximum
}  // namespace quantum

// Quantum arithmetic coding model
struct CRATE_EXPORT quantum_model {
    static constexpr size_t MAX_SYMBOLS = 65;

    struct CRATE_EXPORT symbol_info {
        u16 symbol = 0;
        u16 cumfreq = 0;
    };

    std::array<symbol_info, MAX_SYMBOLS> symbols{};
    unsigned entries = 0;
    unsigned shifts_left = 4;

    void init(u16 start, unsigned len);

    void update(unsigned i);
};

class CRATE_EXPORT quantum_decompressor : public decompressor {
public:
    /// Create a Quantum decompressor with validation
    /// @param window_bits Window size in bits (10-21)
    /// @return Decompressor or error if window_bits is invalid
    static result_t<std::unique_ptr<quantum_decompressor>> create(unsigned window_bits) {
        if (window_bits < quantum::MIN_WINDOW_BITS || window_bits > quantum::MAX_WINDOW_BITS) {
            return std::unexpected(error{error_code::InvalidParameter, "Quantum window_bits must be 10-21"});
        }
        return std::unique_ptr<quantum_decompressor>(new quantum_decompressor(window_bits));
    }

    /// Constructor (prefer using create() for validation)
    /// @param window_bits Window size in bits (10-21, unchecked)
    explicit quantum_decompressor(unsigned window_bits) : window_size_(1u << window_bits), window_(window_size_, 0) {
        init_state();
    }

    result_t<size_t> decompress(byte_span input, mutable_byte_span output) override {
        if (input.size() < 2) {
            return std::unexpected(error{error_code::InputBufferUnderflow});
        }

        // Initialize arithmetic decoder
        pos_ = 0;
        data_ = input;
        C_ = (static_cast<u16>(input[0]) << 8) | input[1];
        pos_ = 2;
        bit_pos_ = 0;
        H_ = 0xFFFF;
        L_ = 0;

        size_t out_pos = 0;

        while (out_pos < output.size()) {
            auto selector = get_symbol(model7_);
            if (!selector) {
                if (out_pos > 0)
                    break;  // EOF after data is ok
                return std::unexpected(selector.error());
            }

            if (*selector < 4) {
                // Literal byte
                quantum_model* model;
                switch (*selector) {
                    case 0:
                        model = &model0_;
                        break;
                    case 1:
                        model = &model1_;
                        break;
                    case 2:
                        model = &model2_;
                        break;
                    default:
                        model = &model3_;
                        break;
                }

                auto sym = get_symbol(*model);
                if (!sym)
                    return std::unexpected(sym.error());

                output[out_pos++] = static_cast<u8>(*sym);
                window_[window_pos_++ & (window_size_ - 1)] = static_cast<u8>(*sym);
            } else {
                // Match
                unsigned match_length;
                u32 match_offset;

                if (*selector == 4) {
                    match_length = 3;
                    auto sym = get_symbol(model4_);
                    if (!sym)
                        return std::unexpected(sym.error());
                    auto off = read_offset(*sym);
                    if (!off)
                        return std::unexpected(off.error());
                    match_offset = *off;
                } else if (*selector == 5) {
                    match_length = 4;
                    auto sym = get_symbol(model5_);
                    if (!sym)
                        return std::unexpected(sym.error());
                    auto off = read_offset(*sym);
                    if (!off)
                        return std::unexpected(off.error());
                    match_offset = *off;
                } else {
                    auto len_sym = get_symbol(model6len_);
                    if (!len_sym)
                        return std::unexpected(len_sym.error());
                    match_length = *len_sym + 5;

                    auto off_sym = get_symbol(model6_);
                    if (!off_sym)
                        return std::unexpected(off_sym.error());
                    auto off = read_offset(*off_sym);
                    if (!off)
                        return std::unexpected(off.error());
                    match_offset = *off;
                }

                // Copy match
                for (unsigned i = 0; i < match_length && out_pos < output.size(); i++) {
                    u8 value = window_[(window_pos_ - match_offset) & (window_size_ - 1)];
                    output[out_pos++] = value;
                    window_[window_pos_++ & (window_size_ - 1)] = value;
                }
            }
        }

        return out_pos;
    }

    void reset() override { init_state(); }

private:
    void init_state() {
        window_pos_ = 0;
        std::fill(window_.begin(), window_.end(), 0);

        model0_.init(0, 64);
        model1_.init(64, 64);
        model2_.init(128, 64);
        model3_.init(192, 64);
        model4_.init(0, 24);
        model5_.init(0, 36);
        model6_.init(0, 42);
        model6len_.init(0, 27);
        model7_.init(0, 7);
    }
    result_t<u16> get_symbol(quantum_model& model) {
        u32 range = static_cast<u32>(H_ - L_) + 1U;
        u32 symf = (static_cast<u32>(C_ - L_ + 1) * model.symbols[0].cumfreq - 1U) / range;

        unsigned i = 1;
        while (i < model.entries && model.symbols[i].cumfreq > symf) {
            i++;
        }

        u16 symbol = model.symbols[i - 1].symbol;

        u32 total = model.symbols[0].cumfreq;
        H_ = static_cast<u16>(L_ + (model.symbols[i - 1].cumfreq * range) / total - 1);
        L_ = static_cast<u16>(L_ + (model.symbols[i].cumfreq * range) / total);

        model.update(i - 1);

        // Renormalize
        while (true) {
            if ((L_ & 0x8000) != (H_ & 0x8000)) {
                if ((L_ & 0x4000) && !(H_ & 0x4000)) {
                    C_ ^= 0x4000;
                    L_ &= 0x3FFF;
                    H_ |= 0x4000;
                } else {
                    break;
                }
            }

            L_ <<= 1;
            H_ = (H_ << 1) | 1;

            u8 bit = 0;
            if (pos_ < data_.size()) {
                bit = (data_[pos_] >> (7 - bit_pos_)) & 1;
                if (++bit_pos_ == 8) {
                    bit_pos_ = 0;
                    pos_++;
                }
            }
            C_ = (C_ << 1) | bit;
        }

        return symbol;
    }

    result_t<u32> read_offset(unsigned sym) {
        // Offset decoding
        if (sym < 2)
            return sym + 1;

        unsigned extra = (sym - 2) / 2 + 1;
        u32 base = (2 + ((sym - 2) & 1)) << extra;

        // Read extra bits
        u32 extra_val = 0;
        for (unsigned i = 0; i < extra; i++) {
            u8 bit = 0;
            if (pos_ < data_.size()) {
                bit = (data_[pos_] >> (7 - bit_pos_)) & 1;
                if (++bit_pos_ == 8) {
                    bit_pos_ = 0;
                    pos_++;
                }
            }
            extra_val = (extra_val << 1) | bit;
        }

        return base + extra_val;
    }

    u32 window_size_ = 0;
    byte_vector window_;
    u32 window_pos_ = 0;

    byte_span data_;
    size_t pos_ = 0;
    unsigned bit_pos_ = 0;

    u16 H_ = 0, L_ = 0, C_ = 0;

    quantum_model model0_, model1_, model2_, model3_;
    quantum_model model4_, model5_, model6_, model6len_, model7_;
};

}  // namespace crate
