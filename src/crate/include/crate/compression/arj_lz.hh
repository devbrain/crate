#pragma once

#include <array>
#include <crate/core/decompressor.hh>
#include <crate/core/types.hh>

namespace crate {

// ARJ Method 4 LZ77 decompressor - streaming implementation
// This is a custom LZ77 algorithm with variable-length encoding for lengths and offsets
class CRATE_EXPORT arj_method4_decompressor : public decompressor {
public:
    static constexpr size_t WINDOW_SIZE = 16384;  // 16KB window
    static constexpr size_t WINDOW_MASK = WINDOW_SIZE - 1;

    explicit arj_method4_decompressor(bool old_format = false);

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    [[nodiscard]] bool supports_streaming() const override { return true; }

    void reset() override;

private:
    void init_state();

    // Streaming bit reader interface
    bool try_peek_bits(const byte*& ptr, const byte* end, unsigned n, u32& out);
    bool try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out);
    void remove_bits(unsigned n);

    // State machine states
    enum class state : u8 {
        READ_LENGTH_PREFIX,    // Reading unary prefix (1s) for length
        READ_LENGTH_EXTRA,     // Reading extra bits for length value
        READ_LITERAL,          // Reading 8-bit literal byte
        READ_OFFSET_PREFIX,    // Reading unary prefix for offset
        READ_OFFSET_EXTRA,     // Reading extra bits for offset value
        COPY_MATCH,            // Copying bytes from window
        DONE                   // Finished
    };

    // Window buffer
    std::array<u8, WINDOW_SIZE> window_{};
    u32 window_pos_ = 0;

    // Bit buffer for streaming
    u64 bit_buffer_ = 0;
    unsigned bits_left_ = 0;

    // State machine
    state state_ = state::READ_LENGTH_PREFIX;

    // Length reading state
    unsigned length_ones_count_ = 0;
    unsigned length_value_ = 0;

    // Match state
    unsigned match_length_ = 0;
    unsigned match_offset_ = 0;
    unsigned match_remaining_ = 0;

    // Offset reading state
    unsigned offset_ones_count_ = 0;

    // Format flag
    bool old_format_ = false;
};

}  // namespace crate
