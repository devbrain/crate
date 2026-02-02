#pragma once

#include <crate/core/decompressor.hh>
#include <array>
#include <vector>

namespace crate {

// ACE LZ77 decompressor - streaming implementation
// Based on ACE 1.0/2.0 LZ77 compression with Huffman coding
class CRATE_EXPORT ace_lz_decompressor : public bounded_decompressor {
public:
    ace_lz_decompressor() {
        init_state();
    }

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    void reset() override {
        init_state();
        clear_expected_output_size();
    }

    void set_dictionary_size(size_t size);

private:
    static constexpr size_t MINDICSIZE = 1 << 10;   // 1KB min
    static constexpr size_t MAXDICSIZE = 1 << 22;   // 4MB max
    static constexpr int MAXDICBITS = 22;
    static constexpr int MAXCODEWIDTH = 11;
    static constexpr int NUMMAINCODES = 260 + MAXDICBITS + 2;  // 284: literals + history + dist codes
    static constexpr int NUMLENCODES = 255;
    static constexpr int TYPECODE = 260 + MAXDICBITS + 1;      // 283
    static constexpr u32 MAXDISTATLEN2 = 255;
    static constexpr u32 MAXDISTATLEN3 = 8191;
    static constexpr int WIDTHWIDTHBITS = 3;
    static constexpr int MAXWIDTHWIDTH = 7;

    // Huffman tree structure
    struct huffman_tree {
        std::vector<u16> codes;
        std::vector<u8> widths;
        int max_width = 0;

        void clear() {
            codes.clear();
            widths.clear();
            max_width = 0;
        }
    };

    enum class state : u8 {
        // Tree reading states
        READ_MAIN_TREE_HEADER,
        READ_MAIN_TREE_WIDTH_WIDTHS,
        BUILD_MAIN_WIDTH_TREE,
        READ_MAIN_TREE_WIDTHS,
        BUILD_MAIN_TREE,
        READ_LEN_TREE_HEADER,
        READ_LEN_TREE_WIDTH_WIDTHS,
        BUILD_LEN_WIDTH_TREE,
        READ_LEN_TREE_WIDTHS,
        BUILD_LEN_TREE,
        READ_SYMS_COUNT,

        // Decompression states
        READ_MAIN_SYMBOL,
        OUTPUT_LITERAL,
        READ_LEN_SYMBOL_HIST,
        READ_DIST_BITS,
        READ_LEN_SYMBOL_NEW,
        COPY_MATCH,
        READ_MODE_TYPE,
        READ_MODE_DELTA,
        READ_MODE_EXE,

        DONE
    };

    void init_state();

    // Streaming bit reader
    bool try_peek_bits(const byte*& ptr, const byte* end, int n, u32& out);
    bool try_read_bits(const byte*& ptr, const byte* end, int n, u32& out);
    void remove_bits(int n);

    // Huffman helpers
    bool try_read_huffman_symbol(const byte*& ptr, const byte* end,
                                  const huffman_tree& tree, u16& symbol);
    result_t<bool> build_huffman_tree(std::vector<u8>& widths, int max_width, huffman_tree& tree);

    // State machine
    state state_ = state::READ_MAIN_TREE_HEADER;

    // Bit buffer
    u64 bit_buffer_ = 0;
    unsigned bits_left_ = 0;

    // Dictionary size
    size_t dic_size_ = MINDICSIZE;

    // Huffman trees
    huffman_tree main_tree_;
    huffman_tree len_tree_;
    huffman_tree width_tree_;  // Temporary for reading other trees

    // Tree reading state
    int tree_num_widths_ = 0;
    int tree_lower_width_ = 0;
    int tree_upper_width_ = 0;
    int tree_width_index_ = 0;
    std::vector<u8> tree_width_widths_;
    std::vector<u8> tree_widths_;

    // Symbols remaining in current block
    int syms_to_read_ = 0;

    // Current symbol being processed
    u16 current_symbol_ = 0;

    // Distance history (4 entries)
    std::array<u32, 4> dist_hist_ = {0, 0, 0, 0};

    // Current match state
    u32 copy_dist_ = 0;
    size_t copy_len_ = 0;
    size_t copy_pos_ = 0;

    // Output window for back-references
    std::vector<u8> window_;
    size_t window_pos_ = 0;
};

void ace_lz_debug(bool enable);

}  // namespace crate
