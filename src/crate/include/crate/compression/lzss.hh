#pragma once

#include <crate/core/decompressor.hh>
#include <crate/core/bitstream.hh>
#include <crate/core/types.hh>

namespace crate {

// LZSS decompressor for SZDD format - true streaming implementation
class CRATE_EXPORT szdd_lzss_decompressor : public decompressor {
public:
    static constexpr size_t WINDOW_SIZE = 4096;
    static constexpr size_t WINDOW_MASK = WINDOW_SIZE - 1;
    static constexpr size_t INITIAL_POS = WINDOW_SIZE - 16;
    static constexpr unsigned MIN_MATCH = 3;
    static constexpr unsigned MAX_MATCH = 18;

    szdd_lzss_decompressor();

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    void reset() override;

private:
    void init_state();

    // State machine states
    enum class state : u8 {
        READ_CONTROL,   // Need to read control byte
        READ_LITERAL,   // Need to read literal byte (bit was 1)
        READ_MATCH_LO,  // Need to read match low byte (bit was 0)
        READ_MATCH_HI,  // Need to read match high byte
        COPY_MATCH,     // Copying bytes from window
        DONE            // Decompression complete
    };

    // Window buffer
    std::array<u8, WINDOW_SIZE> window_{};
    u32 window_pos_ = INITIAL_POS;

    // State machine
    state state_ = state::READ_CONTROL;
    u8 control_byte_ = 0;       // Current control byte
    u8 current_bit_ = 0;        // Which bit we're processing (0-7)

    // Match state (for COPY_MATCH)
    u8 match_lo_ = 0;           // Low byte of match reference
    u16 match_pos_ = 0;         // Position in window to copy from
    u8 match_remaining_ = 0;    // Bytes remaining to copy

    // For tracking if we've seen any input (to detect empty streams)
    bool started_ = false;
};

// LZSS + Huffman decompressor for KWAJ format
class CRATE_EXPORT kwaj_lzss_decompressor : public decompressor {
public:
    static constexpr size_t WINDOW_SIZE = 4096;
    static constexpr size_t WINDOW_MASK = WINDOW_SIZE - 1;

    kwaj_lzss_decompressor() { init_state(); }

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override {
        // KWAJ LZSS decompression requires all input data at once
        if (!input_finished) {
            return stream_result::need_input(0, 0);
        }

        lsb_bitstream bs(input);
        size_t out_pos = 0;

        while (!bs.at_end() && out_pos < output.size()) {
            auto is_literal = bs.read_bit();
            if (!is_literal) {
                if (out_pos > 0)
                    break;  // EOF after data
                return std::unexpected(is_literal.error());
            }

            if (*is_literal) {
                auto value = bs.read_byte();
                if (!value)
                    return std::unexpected(value.error());
                output[out_pos++] = *value;
                window_[window_pos_++ & WINDOW_MASK] = *value;
            } else {
                // Read match length and offset
                auto len_bits = bs.read_bits(4);
                if (!len_bits)
                    return std::unexpected(len_bits.error());
                unsigned match_len = *len_bits + 3;

                auto off_bits = bs.read_bits(12);
                if (!off_bits)
                    return std::unexpected(off_bits.error());
                unsigned match_off = *off_bits;

                for (unsigned i = 0; i < match_len && out_pos < output.size(); i++) {
                    u8 value = window_[(window_pos_ - match_off - 1) & WINDOW_MASK];
                    output[out_pos++] = value;
                    window_[window_pos_++ & WINDOW_MASK] = value;
                }
            }
        }

        return stream_result::done(input.size(), out_pos);
    }

    void reset() override { init_state(); }

private:
    void init_state() {
        window_pos_ = 0;
        std::fill(window_.begin(), window_.end(), 0);
    }
    std::array<u8, WINDOW_SIZE> window_{};
    u32 window_pos_ = 0;
};

}  // namespace crate
