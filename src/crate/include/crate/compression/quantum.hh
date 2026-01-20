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

class CRATE_EXPORT quantum_decompressor : public bounded_decompressor {
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

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override {
        const byte* in_ptr = input.data();
        const byte* in_end = input.data() + input.size();
        byte* out_ptr = output.data();
        if (!expected_output_set()) {
            return std::unexpected(error{
                error_code::InvalidParameter,
                "Expected size required for bounded decompression"
            });
        }
        size_t output_limit = output.size();

        auto bytes_read = [&]() -> size_t {
            return static_cast <size_t>(in_ptr - input.data());
        };
        auto bytes_written = [&]() -> size_t {
            return static_cast <size_t>(out_ptr - output.data());
        };
        auto finalize = [&](decode_status status) -> stream_result {
            size_t read = bytes_read();
            size_t written = bytes_written();
            advance_output(written);

            if (expected_output_set() && total_output_written() >= expected_output_size()) {
                state_ = state::DONE;
                return stream_result::done(read, written);
            }

            if (status == decode_status::needs_more_input) {
                return stream_result::need_input(read, written);
            }
            if (status == decode_status::needs_more_output) {
                return stream_result::need_output(read, written);
            }

            state_ = state::DONE;
            return stream_result::done(read, written);
        };

        if (expected_output_set()) {
            size_t written = total_output_written();
            if (written >= expected_output_size()) {
                state_ = state::DONE;
                return stream_result::done(bytes_read(), 0);
            }
            size_t remaining = expected_output_size() - written;
            if (output_limit > remaining) {
                output_limit = remaining;
            }
        }
        byte* out_end = output.data() + output_limit;
        if (out_ptr >= out_end) {
            return finalize(decode_status::needs_more_output);
        }

        while (state_ != state::DONE) {
            switch (state_) {
                case state::READ_INIT: {
                    while (init_bytes_read_ < 2) {
                        if (in_ptr >= in_end) {
                            if (input_finished) {
                                return std::unexpected(error{error_code::InputBufferUnderflow});
                            }
                            goto need_input;
                        }
                        init_word_ = static_cast <u16>((init_word_ << 8) | *in_ptr++);
                        init_bytes_read_++;
                    }
                    C_ = init_word_;
                    L_ = 0;
                    H_ = 0xFFFF;
                    state_ = state::READ_SELECTOR;
                    break;
                }

                case state::READ_SELECTOR: {
                    u16 selector = 0;
                    if (!try_get_symbol(model7_, selector, in_ptr, in_end, input_finished)) {
                        goto need_input;
                    }
                    selector_ = static_cast <u8>(selector);
                    if (selector_ < 4) {
                        state_ = state::READ_LITERAL;
                    } else if (selector_ == 4) {
                        match_length_ = 3;
                        offset_model_ = &model4_;
                        state_ = state::READ_MATCH_OFFSET_SYMBOL;
                    } else if (selector_ == 5) {
                        match_length_ = 4;
                        offset_model_ = &model5_;
                        state_ = state::READ_MATCH_OFFSET_SYMBOL;
                    } else {
                        state_ = state::READ_MATCH_LEN;
                    }
                    break;
                }

                case state::READ_LITERAL: {
                    quantum_model* model = nullptr;
                    switch (selector_) {
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

                    u16 sym = 0;
                    if (!try_get_symbol(*model, sym, in_ptr, in_end, input_finished)) {
                        goto need_input;
                    }
                    literal_value_ = static_cast <u8>(sym);
                    state_ = state::WRITE_LITERAL;
                    break;
                }

                case state::WRITE_LITERAL:
                    if (out_ptr >= out_end) {
                        goto need_output;
                    }
                    *out_ptr++ = literal_value_;
                    window_[window_pos_++ & (window_size_ - 1)] = literal_value_;
                    state_ = state::READ_SELECTOR;
                    break;

                case state::READ_MATCH_LEN: {
                    u16 len_sym = 0;
                    if (!try_get_symbol(model6len_, len_sym, in_ptr, in_end, input_finished)) {
                        goto need_input;
                    }
                    match_length_ = static_cast <unsigned>(len_sym) + 5;
                    offset_model_ = &model6_;
                    state_ = state::READ_MATCH_OFFSET_SYMBOL;
                    break;
                }

                case state::READ_MATCH_OFFSET_SYMBOL: {
                    u16 off_sym = 0;
                    if (!try_get_symbol(*offset_model_, off_sym, in_ptr, in_end, input_finished)) {
                        goto need_input;
                    }
                    if (off_sym < 2) {
                        match_offset_ = static_cast <u32>(off_sym + 1);
                        match_remaining_ = match_length_;
                        state_ = state::COPY_MATCH;
                    } else {
                        u32 off_sym_value = static_cast <u32>(off_sym);
                        offset_extra_bits_ = (off_sym_value - 2) / 2 + 1;
                        offset_base_ = (2u + ((off_sym_value - 2) & 1u)) << offset_extra_bits_;
                        offset_bits_read_ = 0;
                        offset_extra_value_ = 0;
                        state_ = state::READ_OFFSET_BITS;
                    }
                    break;
                }

                case state::READ_OFFSET_BITS:
                    while (offset_bits_read_ < offset_extra_bits_) {
                        u8 bit = 0;
                        if (!try_read_bit(in_ptr, in_end, input_finished, bit)) {
                            goto need_input;
                        }
                        offset_extra_value_ = (offset_extra_value_ << 1) | bit;
                        offset_bits_read_++;
                    }
                    match_offset_ = offset_base_ + offset_extra_value_;
                    match_remaining_ = match_length_;
                    state_ = state::COPY_MATCH;
                    break;

                case state::COPY_MATCH:
                    while (match_remaining_ > 0) {
                        if (out_ptr >= out_end) {
                            goto need_output;
                        }
                        u8 value = window_[(window_pos_ - match_offset_) & (window_size_ - 1)];
                        *out_ptr++ = value;
                        window_[window_pos_++ & (window_size_ - 1)] = value;
                        match_remaining_--;
                    }
                    state_ = state::READ_SELECTOR;
                    break;

                case state::DONE:
                    break;
            }

            if (state_ == state::DONE) {
                break;
            }
        }

        return finalize(decode_status::done);

    need_input:
        if (input_finished) {
            return std::unexpected(error{error_code::InputBufferUnderflow});
        }
        return finalize(decode_status::needs_more_input);

    need_output:
        return finalize(decode_status::needs_more_output);
    }

    void reset() override { init_state(); }

private:
    enum class state : u8 {
        READ_INIT,
        READ_SELECTOR,
        READ_LITERAL,
        WRITE_LITERAL,
        READ_MATCH_LEN,
        READ_MATCH_OFFSET_SYMBOL,
        READ_OFFSET_BITS,
        COPY_MATCH,
        DONE
    };

    void init_state() {
        clear_expected_output_size();
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

        state_ = state::READ_INIT;
        init_word_ = 0;
        init_bytes_read_ = 0;
        bit_buffer_ = 0;
        bits_left_ = 0;
        pending_symbol_ = false;
        pending_symbol_value_ = 0;
        selector_ = 0;
        literal_value_ = 0;
        match_length_ = 0;
        match_offset_ = 0;
        match_remaining_ = 0;
        offset_model_ = &model4_;
        offset_extra_bits_ = 0;
        offset_bits_read_ = 0;
        offset_base_ = 0;
        offset_extra_value_ = 0;
        H_ = 0;
        L_ = 0;
        C_ = 0;
    }

    bool try_read_bit(const byte*& ptr, const byte* end, bool input_finished, u8& out) {
        if (bits_left_ == 0) {
            if (ptr >= end) {
                if (!input_finished) {
                    return false;
                }
                out = 0;
                return true;
            }
            bit_buffer_ = *ptr++;
            bits_left_ = 8;
        }

        out = static_cast <u8>((bit_buffer_ >> (bits_left_ - 1)) & 1);
        bits_left_--;
        return true;
    }

    bool try_renorm(const byte*& ptr, const byte* end, bool input_finished) {
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

            L_ = static_cast <u16>(L_ << 1);
            H_ = static_cast <u16>((H_ << 1) | 1);

            u8 bit = 0;
            if (!try_read_bit(ptr, end, input_finished, bit)) {
                return false;
            }
            C_ = static_cast <u16>((C_ << 1) | bit);
        }

        return true;
    }

    bool try_get_symbol(
        quantum_model& model,
        u16& symbol,
        const byte*& ptr,
        const byte* end,
        bool input_finished
    ) {
        if (!pending_symbol_) {
            u32 range = static_cast <u32>(H_ - L_) + 1U;
            u32 total = model.symbols[0].cumfreq;
            u32 symf = (static_cast <u32>(C_ - L_ + 1) * total - 1U) / range;

            unsigned i = 1;
            while (i < model.entries && model.symbols[i].cumfreq > symf) {
                i++;
            }

            symbol = model.symbols[i - 1].symbol;

            H_ = static_cast <u16>(L_ + (model.symbols[i - 1].cumfreq * range) / total - 1);
            L_ = static_cast <u16>(L_ + (model.symbols[i].cumfreq * range) / total);

            model.update(i - 1);

            pending_symbol_value_ = symbol;
            pending_symbol_ = true;
        }

        if (!try_renorm(ptr, end, input_finished)) {
            return false;
        }

        symbol = pending_symbol_value_;
        pending_symbol_ = false;
        return true;
    }

    u32 window_size_ = 0;
    byte_vector window_;
    u32 window_pos_ = 0;

    state state_ = state::READ_INIT;
    u16 init_word_ = 0;
    unsigned init_bytes_read_ = 0;
    u8 bit_buffer_ = 0;
    unsigned bits_left_ = 0;
    bool pending_symbol_ = false;
    u16 pending_symbol_value_ = 0;

    u8 selector_ = 0;
    u8 literal_value_ = 0;
    unsigned match_length_ = 0;
    u32 match_offset_ = 0;
    unsigned match_remaining_ = 0;
    quantum_model* offset_model_ = nullptr;

    unsigned offset_extra_bits_ = 0;
    unsigned offset_bits_read_ = 0;
    u32 offset_base_ = 0;
    u32 offset_extra_value_ = 0;

    u16 H_ = 0, L_ = 0, C_ = 0;

    quantum_model model0_, model1_, model2_, model3_;
    quantum_model model4_, model5_, model6_, model6len_, model7_;
};

}  // namespace crate
