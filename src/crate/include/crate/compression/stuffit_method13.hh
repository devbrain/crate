#pragma once

#include <crate/core/decompressor.hh>
#include <array>
#include <vector>

namespace crate {

// StuffIt Method 13: LZSS + Huffman compression
// Uses three Huffman tables: first_code, second_code, offset_code
// Plus a 64KB sliding window for LZSS matches
class CRATE_EXPORT stuffit_method13_decompressor : public decompressor {
public:
    stuffit_method13_decompressor();

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    [[nodiscard]] bool supports_streaming() const override { return true; }

    void reset() override;

private:
    static constexpr size_t WINDOW_SIZE = 65536;
    static constexpr size_t MAX_SYMBOLS = 1024;

    // Huffman table for Method 13
    class huffman_table {
    public:
        huffman_table();
        void clear();
        void init_from_lengths(const int* lengths, size_t num_symbols, bool lsb_first = true);
        void init_from_explicit_codes(const int* codes, const int* lengths, size_t num_symbols);

        // Streaming decode - returns symbol, -1 if need input, -2 on error
        template<typename Reader>
        int decode(Reader& reader) const;

        [[nodiscard]] size_t num_symbols() const { return num_symbols_; }

    private:
        void add_code(u32 code, int len, int symbol);

        std::vector<int> tree_;
        size_t num_symbols_ = 0;
        int next_free_ = 2;
    };

    // Bit buffer operations
    bool try_peek_bits(const byte*& ptr, const byte* end, unsigned n, u32& out);
    bool try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out);
    void remove_bits(unsigned n);

    // Dynamic table parsing state
    struct dynamic_parse_state {
        size_t index = 0;
        size_t num_codes = 0;
        int length = 0;
        std::vector<int> lengths;
        int meta_value = -1;  // Current meta-code value being processed
        int repeat_count = 0;
        bool reading_extra = false;
        int extra_bits_needed = 0;
    };

    // Try to parse dynamic table
    // Returns: 1 = complete, 0 = need more input, -1 = error
    int try_parse_dynamic_table(const byte*& ptr, const byte* end,
                                 huffman_table& table, size_t num_codes);

    // State machine
    enum class state : u8 {
        READ_HEADER,
        BUILD_STATIC_TABLES,
        BUILD_META_TABLE,
        PARSE_FIRST_CODE,
        PARSE_SECOND_CODE,
        PARSE_OFFSET_CODE,
        DECODE_SYMBOL,
        READ_MATCH_LENGTH_10,  // 10-bit extended length
        READ_MATCH_LENGTH_15,  // 15-bit extended length
        DECODE_OFFSET,
        READ_OFFSET_EXTRA,
        COPY_MATCH,
        DONE
    };

    state state_ = state::READ_HEADER;

    // Bit buffer (LSB first)
    u32 bit_buffer_ = 0;
    unsigned bits_left_ = 0;

    // Header info
    u8 header_byte_ = 0;
    int code_type_ = 0;
    bool reuse_first_as_second_ = false;
    size_t offset_code_size_ = 0;

    // Huffman tables
    huffman_table meta_code_;
    huffman_table first_code_;
    huffman_table second_code_;
    huffman_table offset_code_;
    bool use_first_code_ = true;  // Toggle between first and second

    // Dynamic parsing state
    dynamic_parse_state dyn_state_;

    // LZSS window
    std::vector<byte> window_;
    size_t window_pos_ = 0;

    // Match state
    size_t match_length_ = 0;
    size_t match_offset_ = 0;
    size_t match_copied_ = 0;
    int offset_bits_ = 0;

    // Decode state
    size_t decode_node_ = 0;  // Current position in tree during decode
};

}  // namespace crate
