#pragma once

#include <crate/core/decompressor.hh>
#include <array>

namespace crate {

// StuffIt Method 2: LZW compression (14-bit variant)
// Based on Unix compress algorithm without the 3-byte header
class CRATE_EXPORT stuffit_lzw_decompressor : public decompressor {
public:
    stuffit_lzw_decompressor();

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    [[nodiscard]] bool supports_streaming() const override { return true; }

    void reset() override;

private:
    static constexpr int MAX_BITS = 14;
    static constexpr int INIT_BITS = 9;
    static constexpr int FIRST_CODE = 257;  // First free entry
    static constexpr int CLEAR_CODE = 256;  // Table clear code
    static constexpr size_t TABLE_SIZE = 1 << MAX_BITS;
    static constexpr size_t STACK_SIZE = TABLE_SIZE;

    // Get next code from input
    // Returns: code value, or -1 if need more input, or -2 on error
    int get_code(const byte*& ptr, const byte* end);

    // Reset dictionary to initial state
    void clear_table();

    // State machine
    enum class state : u8 {
        READ_FIRST_CODE,    // Read the first code
        READ_CODE,          // Read subsequent codes
        OUTPUT_STRING,      // Outputting decoded string
        DONE
    };

    state state_ = state::READ_FIRST_CODE;

    // LZW tables
    std::array<u16, TABLE_SIZE> tab_prefix_;
    std::array<u8, TABLE_SIZE> tab_suffix_;

    // Output stack (strings are built in reverse)
    std::array<u8, STACK_SIZE> stack_;
    size_t stack_ptr_ = 0;       // Next position to write in stack
    size_t stack_out_ptr_ = 0;   // Next position to read from stack

    // Decoding state
    int n_bits_ = INIT_BITS;     // Current code size
    int maxcode_ = 0;            // Max code for current n_bits
    int max_maxcode_ = 0;        // Max code for MAX_BITS
    int free_ent_ = FIRST_CODE;  // Next free table entry
    bool clear_flg_ = false;     // Just cleared table

    int oldcode_ = -1;           // Previous code
    int finchar_ = 0;            // First char of previous string
    int incode_ = 0;             // Current code being processed

    // Bit buffer
    std::array<u8, MAX_BITS> gbuf_;
    int bit_offset_ = 0;
    int bits_in_buffer_ = 0;
    size_t gbuf_bytes_ = 0;      // Bytes currently in gbuf_
};

}  // namespace crate
