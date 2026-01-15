#pragma once

#include <crate/core/decompressor.hh>
#include <crate/core/huffman.hh>
#include <array>
#include <vector>

namespace crate {

// MSZIP constants
constexpr size_t MSZIP_BLOCK_SIZE = 32768;

// Deflate extra bits tables
namespace deflate {
    constexpr std::array<u8, 29> length_extra_bits = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };

    constexpr std::array<u16, 29> length_base = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };

    constexpr std::array<u8, 30> distance_extra_bits = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };

    constexpr std::array<u16, 30> distance_base = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577
    };

    // Code length alphabet order
    constexpr std::array<u8, 19> code_length_order = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
}

class mszip_decompressor : public decompressor {
public:
    mszip_decompressor();

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    void reset() override;

private:
    // State machine states
    enum class state {
        READ_SIGNATURE_0,       // Read first byte of "CK"
        READ_SIGNATURE_1,       // Read second byte
        READ_BLOCK_HEADER,      // Read final flag + block type (3 bits)

        // Stored block states
        ALIGN_STORED,           // Align to byte boundary
        READ_STORED_LEN_LO,
        READ_STORED_LEN_HI,
        READ_STORED_NLEN_LO,
        READ_STORED_NLEN_HI,
        COPY_STORED,

        // Dynamic Huffman states
        READ_HLIT,
        READ_HDIST,
        READ_HCLEN,
        READ_CODE_LENGTHS,      // Read code length code lengths
        BUILD_CODE_LEN_TABLE,   // Build code length decoder
        DECODE_LENGTH_CODES,    // Decode literal/length and distance lengths
        BUILD_TABLES,           // Build literal and distance tables

        // Common decompression states
        BUILD_FIXED_TABLES,
        DECODE_SYMBOL,          // Decode literal/length symbol
        READ_LENGTH_EXTRA,
        DECODE_DISTANCE,
        READ_DISTANCE_EXTRA,
        COPY_MATCH,

        DONE
    };

    void init_state();
    void update_history(u8 value);
    u8 get_history(size_t distance) const;
    void build_fixed_tables();

    // Try to read bits from input, returns false if not enough data
    bool try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out);
    bool try_read_byte(const byte*& ptr, const byte* end, u8& out);

    // Try to decode a Huffman symbol, returns false if not enough data
    template<size_t N>
    bool try_decode(const byte*& ptr, const byte* end,
                    huffman_decoder<N>& decoder, u16& out);

    // Persistent state
    state state_ = state::READ_SIGNATURE_0;

    // Bit buffer (persists across calls)
    u64 bit_buffer_ = 0;
    unsigned bits_left_ = 0;

    // Block state
    bool final_block_ = false;
    u8 block_type_ = 0;

    // Stored block state
    u16 stored_len_ = 0;
    u16 stored_remaining_ = 0;

    // Dynamic table building state
    u16 hlit_ = 0;
    u16 hdist_ = 0;
    u16 hclen_ = 0;
    u16 code_len_idx_ = 0;
    std::array<u8, 19> code_len_lengths_{};
    huffman_decoder<19> code_len_decoder_;
    std::vector<u8> all_lengths_;
    size_t length_decode_idx_ = 0;
    u8 last_length_ = 0;
    u16 repeat_count_ = 0;
    u8 repeat_value_ = 0;

    // Decompression state
    u16 current_symbol_ = 0;
    u16 match_length_ = 0;
    u16 match_distance_ = 0;
    u16 match_remaining_ = 0;

    // Huffman tables
    literal_decoder literal_decoder_;
    distance_decoder distance_decoder_;

    // History buffer
    std::array<u8, MSZIP_BLOCK_SIZE> history_{};
    size_t history_pos_ = 0;
};

} // namespace crate
