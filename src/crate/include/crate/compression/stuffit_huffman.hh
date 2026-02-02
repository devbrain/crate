#pragma once

#include <crate/core/decompressor.hh>
#include <vector>

namespace crate {

// StuffIt Method 3: Simple Huffman
// Binary tree encoded in bitstream (MSB-first), then data decoded using that tree
class CRATE_EXPORT stuffit_huffman_decompressor : public decompressor {
public:
    stuffit_huffman_decompressor() = default;

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    [[nodiscard]] bool supports_streaming() const override { return true; }

    void reset() override;

private:
    // Huffman tree node
    struct tree_node {
        bool is_leaf = false;
        int value = 0;           // Leaf: byte value
        size_t zero_child = 0;   // Internal: index of left child
        size_t one_child = 0;    // Internal: index of right child
    };

    // Try to read and consume bits from the bit buffer
    bool try_peek_bits(const byte*& ptr, const byte* end, unsigned n, u32& out);
    void remove_bits(unsigned n);

    // Build tree recursively from bitstream
    // Returns: true if complete, false if needs more input, error on invalid
    result_t<bool> try_parse_node(const byte*& ptr, const byte* end);

    // State machine
    enum class state : u8 {
        BUILD_TREE,   // Parsing tree from bitstream
        DECODE_DATA   // Decoding symbols
    };

    state state_ = state::BUILD_TREE;

    // Bit buffer (MSB-first for StuffIt Huffman)
    u32 bit_buffer_ = 0;
    unsigned bits_left_ = 0;

    // Tree
    std::vector<tree_node> tree_;

    // Tree building state
    struct build_frame {
        size_t node_idx;
        u8 phase;  // 0=start, 1=need_zero_child, 2=need_one_child
    };
    std::vector<build_frame> build_stack_;

    // Decode state
    size_t decode_node_ = 0;  // Current position in tree during decode
};

}  // namespace crate
