#pragma once

#include <algorithm>
#include <array>
#include <crate/core/decompressor.hh>
#include <crate/core/bitstream.hh>
#include <crate/core/types.hh>
#include <vector>

namespace crate {

// LZH format variant
enum class lzh_format {
    LH5,  // 8KB window
    LH6,  // 32KB window (used by ARJ method 1-3)
    LH7   // 64KB window (used by ARJZ)
};

// Huffman tree for LZH decompression (streaming-capable)
class CRATE_EXPORT lzh_huffman_tree {
public:
    static constexpr unsigned MAX_CODES = 512;
    static constexpr unsigned MAX_CODE_LENGTH = 16;

    lzh_huffman_tree() = default;

    void clear();

    bool build_from_lengths(const std::vector<unsigned>& lengths);

    // Build a tree with a single value (for ncodes=0 case)
    void build_single(unsigned value);

    // Try to decode a symbol using streaming bit reader
    // Returns: true if symbol decoded, false if needs more input
    // On success, 'out' contains the decoded symbol
    // The reader must provide: try_peek_bits(n, out), remove_bits(n)
    template<typename Reader>
    result_t<bool> try_decode(Reader& reader, u16& out) {
        if (is_single_) {
            out = static_cast<u16>(single_value_);
            return true;
        }

        if (max_length_ == 0) {
            return std::unexpected(error{error_code::InvalidHuffmanTable, "Empty Huffman tree"});
        }

        // Try lookup table first (8 bits) - but only if we have enough bits
        u32 bits = 0;
        if (reader.try_peek_bits(8, bits)) {
            u16 entry = lookup_[bits & 0xFF];
            if (entry != 0xFFFF) {
                unsigned len = entry >> 12;
                unsigned value = entry & 0x0FFF;
                reader.remove_bits(len);
                out = static_cast<u16>(value);
                return true;
            }
        }

        // Slow path: bit-by-bit decoding (also handles end-of-stream with < 8 bits)
        // Read one bit at a time to handle end-of-stream gracefully
        unsigned code = 0;
        for (unsigned len = 1; len <= max_length_; len++) {
            u32 bit = 0;
            if (!reader.try_peek_bits(1, bit)) {
                return false;  // Need more input (just 1 bit)
            }
            reader.remove_bits(1);
            code = (code << 1) | (bit & 1);

            // Search for matching code
            for (unsigned i = 0; i < codes_.size(); i++) {
                if (lengths_[i] == len && codes_[i] == code) {
                    out = static_cast<u16>(i);
                    return true;
                }
            }
        }

        return std::unexpected(error{error_code::InvalidHuffmanTable, "Failed to decode Huffman symbol"});
    }

    // Legacy decode for non-streaming use
    template<typename Bitstream>
    result_t<unsigned> decode(Bitstream& bs) {
        if (is_single_) {
            return single_value_;
        }

        // Try lookup table first (8 bits)
        auto peek = bs.peek_bits(8);
        if (!peek)
            return std::unexpected(peek.error());

        u16 entry = lookup_[*peek];
        if (entry != 0xFFFF) {
            unsigned len = entry >> 12;
            unsigned value = entry & 0x0FFF;
            bs.remove_bits(len);
            return value;
        }

        // Slow path: bit-by-bit decoding
        unsigned code = 0;
        for (unsigned len = 1; len <= max_length_; len++) {
            auto bit = bs.read_bit();
            if (!bit)
                return std::unexpected(bit.error());
            code = (code << 1) | (*bit ? 1 : 0);

            // Search for matching code
            for (unsigned i = 0; i < codes_.size(); i++) {
                if (lengths_[i] == len && codes_[i] == code) {
                    return i;
                }
            }
        }

        return std::unexpected(error{error_code::InvalidHuffmanTable, "Failed to decode Huffman symbol"});
    }

private:
    void build_lookup_table();

    std::vector<unsigned> codes_;
    std::vector<unsigned> lengths_;
    std::array<u16, 256> lookup_{};
    unsigned max_length_ = 0;
    unsigned single_value_ = 0;
    bool is_single_ = false;
};

// LZH (LH5/LH6/LH7) decompressor - true streaming implementation
// Used by ARJ methods 1-3 and LHA archives
class CRATE_EXPORT lzh_decompressor : public decompressor {
public:
    explicit lzh_decompressor(lzh_format format = lzh_format::LH6);

    // Constructor with explicit window size parameters for LHA archives
    // LHA uses different window sizes than ARJ for the same method names:
    //   LHA LH5: 16KB window (14 bits), 4 offset bits
    //   LHA LH6: 64KB window (16 bits), 5 offset bits
    //   LHA LH7: 128KB window (17 bits), 5 offset bits
    lzh_decompressor(size_t window_size, unsigned offset_bits, unsigned max_offset_codes);

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
        READ_BLOCK_SIZE,         // Read 16-bit code count
        READ_CL_TREE_NCODES,     // Read 5 bits for codelengths tree size
        READ_CL_TREE_SINGLE,     // Read single value (if ncodes == 0)
        READ_CL_TREE_LENGTH,     // Read a code length (3 bits)
        READ_CL_TREE_LENGTH_EXT, // Read extension bits for code length == 7
        READ_CL_TREE_SKIP,       // Read optional skip after position 2
        BUILD_CL_TREE,           // Build the codelengths tree
        READ_LIT_TREE_NCODES,    // Read 9 bits for literals tree size
        READ_LIT_TREE_SINGLE,    // Read single value (if ncodes == 0)
        READ_LIT_TREE_CODE,      // Decode a code using codelengths tree
        READ_LIT_TREE_SKIP,      // Read skip length for codes 0-2
        BUILD_LIT_TREE,          // Build the literals tree
        READ_OFF_TREE_NCODES,    // Read offset tree size
        READ_OFF_TREE_SINGLE,    // Read single value (if ncodes == 0)
        READ_OFF_TREE_LENGTH,    // Read code lengths for offsets tree
        READ_OFF_TREE_LENGTH_EXT,// Read extension bits for offset code length == 7
        BUILD_OFF_TREE,          // Build the offsets tree
        DECODE_SYMBOL,           // Decode a symbol from literals tree
        DECODE_OFFSET,           // Decode offset for a match
        READ_EXTRA_BITS,         // Read extra bits for offset
        COPY_MATCH,              // Copy bytes from window
        DONE                     // Finished
    };

    // Format parameters
    lzh_format format_ = lzh_format::LH5;
    size_t window_size_ = 0;
    unsigned offsets_bits_ = 0;
    unsigned max_offset_codes_ = 0;

    // Window buffer
    std::vector<u8> window_;
    size_t window_pos_ = 0;

    // Huffman trees
    lzh_huffman_tree codelengths_tree_;
    lzh_huffman_tree literals_tree_;
    lzh_huffman_tree offsets_tree_;

    // Bit buffer for streaming
    u64 bit_buffer_ = 0;
    unsigned bits_left_ = 0;

    // State machine
    state state_ = state::READ_BLOCK_SIZE;

    // Block state
    unsigned block_codes_total_ = 0;    // Total codes in current block
    unsigned block_codes_done_ = 0;     // Codes processed in current block

    // Tree building state
    unsigned tree_ncodes_ = 0;          // Number of codes for current tree
    unsigned tree_index_ = 0;           // Current position in tree building
    std::vector<unsigned> tree_lengths_; // Code lengths being built

    // Code length reading state (for 3-bit + extension)
    unsigned code_length_value_ = 0;    // Current code length being read

    // Skip state
    unsigned skip_code_ = 0;            // Skip code (0, 1, or 2)

    // Match state
    unsigned match_length_ = 0;
    unsigned match_offset_ = 0;
    unsigned match_remaining_ = 0;
    unsigned offset_code_ = 0;
    unsigned extra_bits_needed_ = 0;
};

}  // namespace crate
