#pragma once

#include <crate/core/decompressor.hh>
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

    [[nodiscard]] bool supports_streaming() const override { return true; }

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
    ) override;

    [[nodiscard]] bool supports_streaming() const override { return true; }

    void reset() override { init_state(); }

private:
    bool try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out);

    enum class state : u8 {
        READ_FLAG,
        READ_LITERAL,
        WRITE_LITERAL,
        READ_MATCH_LEN,
        READ_MATCH_OFF,
        COPY_MATCH,
        DONE
    };

    void init_state() {
        window_pos_ = 0;
        std::fill(window_.begin(), window_.end(), 0);
        state_ = state::READ_FLAG;
        bit_buffer_ = 0;
        bits_left_ = 0;
        pending_literal_ = 0;
        match_length_ = 0;
        match_offset_ = 0;
        match_pos_ = 0;
        match_remaining_ = 0;
    }
    std::array <u8, WINDOW_SIZE> window_{};
    u32 window_pos_ = 0;
    state state_ = state::READ_FLAG;
    u64 bit_buffer_ = 0;
    unsigned bits_left_ = 0;
    u8 pending_literal_ = 0;
    u8 match_length_ = 0;
    u16 match_offset_ = 0;
    u32 match_pos_ = 0;
    u16 match_remaining_ = 0;
};

}  // namespace crate
