#pragma once

#include <crate/core/decompressor.hh>
#include <array>
#include <vector>

namespace crate {

// LHA LH5/LH6/LH7 streaming decompressor
// This is specifically for LHA archives, separate from ARJ's LZH format
class CRATE_EXPORT lha_new_decompressor : public decompressor {
public:
    // LH5/LH4: window_bits=14 (16KB), offset_bits=4
    // LH6: window_bits=16 (64KB), offset_bits=5
    // LH7: window_bits=17 (128KB), offset_bits=5
    lha_new_decompressor(unsigned window_bits, unsigned offset_bits);

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    [[nodiscard]] bool supports_streaming() const override { return true; }

    void reset() override;

private:
    static constexpr size_t NUM_CODES = 510;
    static constexpr size_t MAX_TEMP_CODES = 31;
    static constexpr size_t COPY_THRESHOLD = 3;

    void init_state();

    // Streaming bit reader
    bool try_peek_bits(const byte*& ptr, const byte* end, unsigned n, u32& out);
    bool try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out);
    void remove_bits(unsigned n);

    // Read a length value (3 bits + extension for 7+)
    // Returns: true if complete, false if needs more input
    // Sets code_length_value_ with result
    bool try_read_length_value(const byte*& ptr, const byte* end);

    // Huffman tree structure (simplified for LHA)
    struct huffman_tree {
        static constexpr u16 LEAF = 0x8000;
        std::vector<u16> tree;
        u16 single_value = 0;
        bool is_single = false;

        void set_single(u16 value) {
            single_value = value;
            is_single = true;
        }

        bool build(const u8* lengths, size_t count);

        // Returns: symbol on success, -1 if needs more input, -2 on error
        template<typename Reader>
        int read(Reader& reader);
    };

    // State machine
    enum class state : u8 {
        READ_BLOCK_SIZE,

        // Temp tree (codelengths tree)
        READ_TEMP_NCODES,
        READ_TEMP_SINGLE,
        READ_TEMP_LENGTH,
        READ_TEMP_LENGTH_EXT,
        READ_TEMP_SKIP,      // Skip after position 2
        BUILD_TEMP_TREE,

        // Code tree (literals + lengths)
        READ_CODE_NCODES,
        READ_CODE_SINGLE,
        READ_CODE_SYMBOL,    // Decode using temp tree
        READ_CODE_SKIP,      // Handle skip codes 0-2
        BUILD_CODE_TREE,

        // Offset tree
        READ_OFF_NCODES,
        READ_OFF_SINGLE,
        READ_OFF_LENGTH,
        READ_OFF_LENGTH_EXT,
        BUILD_OFF_TREE,

        // Decompression
        DECODE_SYMBOL,
        READ_OFFSET,
        READ_OFFSET_EXTRA,
        COPY_MATCH,

        DONE
    };

    // Format parameters
    unsigned window_bits_ = 14;
    unsigned offset_bits_ = 4;
    size_t window_size_ = 0;
    size_t max_offset_codes_ = 0;

    // Bit buffer
    u32 bit_buffer_ = 0;
    unsigned bits_left_ = 0;

    // State
    state state_ = state::READ_BLOCK_SIZE;

    // Ring buffer
    std::vector<u8> ringbuf_;
    size_t ringbuf_pos_ = 0;

    // Block state
    size_t block_remaining_ = 0;

    // Tree building state
    unsigned tree_ncodes_ = 0;
    unsigned tree_index_ = 0;
    std::array<u8, NUM_CODES> tree_lengths_{};

    // Length reading state (for 3-bit + extension)
    unsigned code_length_value_ = 0;
    bool in_length_ext_ = false;

    // Skip state
    unsigned skip_code_ = 0;

    // Match state
    size_t copy_count_ = 0;
    size_t copy_start_ = 0;
    size_t copy_pos_ = 0;

    // Offset decoding state
    unsigned offset_bits_value_ = 0;

    // Huffman trees
    huffman_tree temp_tree_;
    huffman_tree code_tree_;
    huffman_tree offset_tree_;
};

}  // namespace crate
