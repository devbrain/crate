#include <crate/compression/stuffit_huffman.hh>

namespace crate {

void stuffit_huffman_decompressor::reset() {
    state_ = state::BUILD_TREE;
    bit_buffer_ = 0;
    bits_left_ = 0;
    tree_.clear();
    build_stack_.clear();
    decode_node_ = 0;
}

bool stuffit_huffman_decompressor::try_peek_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
    // MSB-first bit reading for StuffIt Huffman
    while (bits_left_ < n) {
        if (ptr >= end) {
            return false;
        }
        bit_buffer_ = (bit_buffer_ << 8) | *ptr++;
        bits_left_ += 8;
    }
    out = (bit_buffer_ >> (bits_left_ - n)) & ((1u << n) - 1);
    return true;
}

void stuffit_huffman_decompressor::remove_bits(unsigned n) {
    bits_left_ -= n;
    bit_buffer_ &= (1u << bits_left_) - 1;
}

result_t<bool> stuffit_huffman_decompressor::try_parse_node(const byte*& ptr, const byte* end) {
    // Initialize stack if empty
    if (build_stack_.empty()) {
        tree_.push_back({});  // Root node
        build_stack_.push_back({0, 0});
    }

    while (!build_stack_.empty()) {
        auto& frame = build_stack_.back();

        if (frame.phase == 0) {
            // Read bit to determine if leaf or internal
            u32 bit = 0;
            if (!try_peek_bits(ptr, end, 1, bit)) {
                return false;  // Need more input
            }
            remove_bits(1);

            if (bit == 1) {
                // Leaf node - read 8-bit value
                u32 value = 0;
                if (!try_peek_bits(ptr, end, 8, value)) {
                    return false;
                }
                remove_bits(8);

                tree_[frame.node_idx].is_leaf = true;
                tree_[frame.node_idx].value = static_cast<int>(value);
                build_stack_.pop_back();
            } else {
                // Internal node - need to parse children
                frame.phase = 1;  // Next: zero child
            }
        } else if (frame.phase == 1) {
            // Create and parse zero (left) child
            size_t child_idx = tree_.size();
            tree_.push_back({});
            tree_[frame.node_idx].zero_child = child_idx;
            frame.phase = 2;  // After this returns: one child
            build_stack_.push_back({child_idx, 0});
        } else {
            // Create and parse one (right) child
            size_t child_idx = tree_.size();
            tree_.push_back({});
            tree_[frame.node_idx].one_child = child_idx;
            build_stack_.pop_back();  // Done with this node after child completes
            build_stack_.push_back({child_idx, 0});
        }
    }

    return true;  // Tree complete
}

result_t<stream_result> stuffit_huffman_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    const byte* in_ptr = input.data();
    const byte* in_end = input.data() + input.size();
    byte* out_ptr = output.data();
    byte* out_end = output.data() + output.size();

    auto bytes_read = [&]() -> size_t {
        return static_cast<size_t>(in_ptr - input.data());
    };
    auto bytes_written = [&]() -> size_t {
        return static_cast<size_t>(out_ptr - output.data());
    };

    // Build tree if not done
    if (state_ == state::BUILD_TREE) {
        auto result = try_parse_node(in_ptr, in_end);
        if (!result) {
            return std::unexpected(result.error());
        }
        if (!*result) {
            if (input_finished) {
                return std::unexpected(error{error_code::CorruptData, "Incomplete Huffman tree"});
            }
            return stream_result::need_input(bytes_read(), bytes_written());
        }
        state_ = state::DECODE_DATA;
        decode_node_ = 0;
    }

    // Decode data
    while (state_ == state::DECODE_DATA) {
        // Traverse tree until we hit a leaf
        while (decode_node_ < tree_.size() && !tree_[decode_node_].is_leaf) {
            u32 bit = 0;
            if (!try_peek_bits(in_ptr, in_end, 1, bit)) {
                if (input_finished) {
                    // End of input during decode - done
                    return stream_result::done(bytes_read(), bytes_written());
                }
                return stream_result::need_input(bytes_read(), bytes_written());
            }
            remove_bits(1);

            if (bit == 0) {
                decode_node_ = tree_[decode_node_].zero_child;
            } else {
                decode_node_ = tree_[decode_node_].one_child;
            }

            if (decode_node_ >= tree_.size()) {
                return std::unexpected(error{error_code::CorruptData, "Invalid Huffman tree reference"});
            }
        }

        if (decode_node_ >= tree_.size()) {
            return std::unexpected(error{error_code::CorruptData, "Invalid tree node"});
        }

        // Output the leaf value
        if (out_ptr >= out_end) {
            return stream_result::need_output(bytes_read(), bytes_written());
        }

        *out_ptr++ = static_cast<byte>(tree_[decode_node_].value);

        // Reset to root for next symbol
        decode_node_ = 0;
    }

    return stream_result::done(bytes_read(), bytes_written());
}

}  // namespace crate
