#include <crate/compression/lha_new.hh>
#include <algorithm>

namespace crate {

// Huffman tree building (same algorithm as batch decoder)
bool lha_new_decompressor::huffman_tree::build(const u8* lengths, size_t count) {
    is_single = false;
    tree.clear();

    if (count == 0) return false;

    // Count codes at each length
    std::array<unsigned, 17> length_count{};
    unsigned max_length = 0;
    for (size_t i = 0; i < count; i++) {
        if (lengths[i] > 16) return false;
        if (lengths[i] > 0) {
            length_count[lengths[i]]++;
            max_length = std::max(max_length, static_cast<unsigned>(lengths[i]));
        }
    }

    if (max_length == 0) return false;

    // Check for valid Huffman tree
    unsigned code = 0;
    std::array<unsigned, 17> next_code{};
    for (unsigned bits = 1; bits <= max_length; bits++) {
        code = (code + length_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    // Build tree structure
    // Tree is stored as: tree[node] = left child, tree[node+1] = right child
    // LEAF flag indicates a leaf node with symbol value
    tree.resize(2);
    tree[0] = 0;
    tree[1] = 0;

    for (size_t sym = 0; sym < count; sym++) {
        unsigned len = lengths[sym];
        if (len == 0) continue;

        unsigned c = next_code[len]++;
        size_t node = 0;

        for (unsigned bit = len; bit > 1; bit--) {
            unsigned b = (c >> (bit - 1)) & 1;
            size_t child_idx = node + b;

            if (tree[child_idx] == 0) {
                // Create new internal node
                size_t new_node = tree.size();
                tree.push_back(0);
                tree.push_back(0);
                tree[child_idx] = static_cast<u16>(new_node);
            }
            node = tree[child_idx];
        }

        // Set leaf
        unsigned b = c & 1;
        tree[node + b] = static_cast<u16>(sym) | LEAF;
    }

    return true;
}

template<typename Reader>
int lha_new_decompressor::huffman_tree::read(Reader& reader) {
    if (is_single) {
        return static_cast<int>(single_value);
    }

    if (tree.empty()) {
        return -2;  // Error: empty tree
    }

    size_t node = 0;
    while (true) {
        u32 bit = 0;
        if (!reader.try_peek_bits(1, bit)) {
            return -1;  // Need more input
        }
        reader.remove_bits(1);

        size_t child_idx = node + (bit & 1);
        if (child_idx >= tree.size()) {
            return -2;  // Error
        }

        u16 child = tree[child_idx];
        if (child & LEAF) {
            return child & ~LEAF;
        }

        node = child;
        if (node == 0 || node >= tree.size()) {
            return -2;  // Error
        }
    }
}

lha_new_decompressor::lha_new_decompressor(unsigned window_bits, unsigned offset_bits)
    : window_bits_(window_bits),
      offset_bits_(offset_bits),
      window_size_(1u << window_bits),
      max_offset_codes_((1u << offset_bits) - 1) {
    init_state();
}

void lha_new_decompressor::reset() {
    init_state();
}

void lha_new_decompressor::init_state() {
    ringbuf_.resize(window_size_);
    std::fill(ringbuf_.begin(), ringbuf_.end(), ' ');
    ringbuf_pos_ = 0;

    bit_buffer_ = 0;
    bits_left_ = 0;

    state_ = state::READ_BLOCK_SIZE;
    block_remaining_ = 0;

    tree_ncodes_ = 0;
    tree_index_ = 0;
    tree_lengths_.fill(0);

    code_length_value_ = 0;
    in_length_ext_ = false;
    skip_code_ = 0;

    copy_count_ = 0;
    copy_start_ = 0;
    copy_pos_ = 0;

    offset_bits_value_ = 0;

    temp_tree_ = huffman_tree{};
    code_tree_ = huffman_tree{};
    offset_tree_ = huffman_tree{};
}

bool lha_new_decompressor::try_peek_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
    if (n == 0) {
        out = 0;
        return true;
    }

    // Fill bit buffer (MSB first, same as batch decoder)
    while (bits_left_ < n) {
        if (ptr >= end) {
            return false;
        }
        bit_buffer_ |= static_cast<u32>(*ptr++) << (24 - bits_left_);
        bits_left_ += 8;
    }

    out = bit_buffer_ >> (32 - n);
    return true;
}

bool lha_new_decompressor::try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
    if (!try_peek_bits(ptr, end, n, out)) {
        return false;
    }
    remove_bits(n);
    return true;
}

void lha_new_decompressor::remove_bits(unsigned n) {
    bit_buffer_ <<= n;
    bits_left_ -= n;
}

bool lha_new_decompressor::try_read_length_value(const byte*& ptr, const byte* end) {
    if (!in_length_ext_) {
        u32 len = 0;
        if (!try_read_bits(ptr, end, 3, len)) {
            return false;
        }
        code_length_value_ = len;
        if (len == 7) {
            in_length_ext_ = true;
        }
    }

    if (in_length_ext_) {
        while (true) {
            u32 bit = 0;
            if (!try_read_bits(ptr, end, 1, bit)) {
                return false;
            }
            if (bit == 0) {
                break;
            }
            code_length_value_++;
            if (code_length_value_ > 16) {
                // Error: code length too large
                return false;
            }
        }
        in_length_ext_ = false;
    }

    return true;
}

result_t<stream_result> lha_new_decompressor::decompress_some(
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

    // Bit reader wrapper for Huffman tree
    struct bit_reader {
        lha_new_decompressor& owner;
        const byte*& ptr;
        const byte* end;

        bool try_peek_bits(unsigned n, u32& out) {
            return owner.try_peek_bits(ptr, end, n, out);
        }
        void remove_bits(unsigned n) {
            owner.remove_bits(n);
        }
    };
    bit_reader reader{*this, in_ptr, in_end};

    while (state_ != state::DONE) {
        switch (state_) {
            case state::READ_BLOCK_SIZE: {
                u32 len = 0;
                if (!try_read_bits(in_ptr, in_end, 16, len)) {
                    if (input_finished && bytes_written() > 0) {
                        state_ = state::DONE;
                        return stream_result::done(bytes_read(), bytes_written());
                    }
                    if (input_finished) {
                        state_ = state::DONE;
                        return stream_result::done(bytes_read(), bytes_written());
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                block_remaining_ = len;
                if (block_remaining_ == 0) {
                    if (bytes_written() > 0) {
                        state_ = state::DONE;
                        return stream_result::done(bytes_read(), bytes_written());
                    }
                }
                state_ = state::READ_TEMP_NCODES;
                break;
            }

            case state::READ_TEMP_NCODES: {
                u32 n = 0;
                if (!try_read_bits(in_ptr, in_end, 5, n)) {
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                tree_ncodes_ = n;
                if (tree_ncodes_ == 0) {
                    state_ = state::READ_TEMP_SINGLE;
                } else {
                    if (tree_ncodes_ > MAX_TEMP_CODES) tree_ncodes_ = MAX_TEMP_CODES;
                    tree_index_ = 0;
                    tree_lengths_.fill(0);
                    in_length_ext_ = false;
                    state_ = state::READ_TEMP_LENGTH;
                }
                break;
            }

            case state::READ_TEMP_SINGLE: {
                u32 code = 0;
                if (!try_read_bits(in_ptr, in_end, 5, code)) {
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                temp_tree_.set_single(static_cast<u16>(code));
                state_ = state::READ_CODE_NCODES;
                break;
            }

            case state::READ_TEMP_LENGTH: {
                while (tree_index_ < tree_ncodes_) {
                    if (!try_read_length_value(in_ptr, in_end)) {
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    tree_lengths_[tree_index_] = static_cast<u8>(code_length_value_);
                    tree_index_++;

                    // After position 2 (indices 0,1,2 read), read skip
                    if (tree_index_ == 3 && tree_index_ < tree_ncodes_) {
                        state_ = state::READ_TEMP_SKIP;
                        break;
                    }
                }
                if (tree_index_ >= tree_ncodes_ && state_ == state::READ_TEMP_LENGTH) {
                    state_ = state::BUILD_TEMP_TREE;
                }
                break;
            }

            case state::READ_TEMP_LENGTH_EXT: {
                // Handled within try_read_length_value
                state_ = state::READ_TEMP_LENGTH;
                break;
            }

            case state::READ_TEMP_SKIP: {
                u32 skip = 0;
                if (!try_read_bits(in_ptr, in_end, 2, skip)) {
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                // Skip positions (leave as 0)
                for (unsigned i = 0; i < skip && tree_index_ < tree_ncodes_; i++) {
                    tree_lengths_[tree_index_] = 0;
                    tree_index_++;
                }
                in_length_ext_ = false;
                state_ = state::READ_TEMP_LENGTH;
                break;
            }

            case state::BUILD_TEMP_TREE: {
                if (!temp_tree_.build(tree_lengths_.data(), tree_ncodes_)) {
                    return std::unexpected(error{error_code::InvalidHuffmanTable, "Failed to build temp tree"});
                }
                state_ = state::READ_CODE_NCODES;
                break;
            }

            case state::READ_CODE_NCODES: {
                u32 n = 0;
                if (!try_read_bits(in_ptr, in_end, 9, n)) {
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                tree_ncodes_ = n;
                if (tree_ncodes_ == 0) {
                    state_ = state::READ_CODE_SINGLE;
                } else {
                    if (tree_ncodes_ > NUM_CODES) tree_ncodes_ = NUM_CODES;
                    tree_index_ = 0;
                    tree_lengths_.fill(0);
                    state_ = state::READ_CODE_SYMBOL;
                }
                break;
            }

            case state::READ_CODE_SINGLE: {
                u32 code = 0;
                if (!try_read_bits(in_ptr, in_end, 9, code)) {
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                code_tree_.set_single(static_cast<u16>(code));
                state_ = state::READ_OFF_NCODES;
                break;
            }

            case state::READ_CODE_SYMBOL: {
                while (tree_index_ < tree_ncodes_) {
                    int code = temp_tree_.read(reader);
                    if (code == -1) {
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    if (code < 0) {
                        return std::unexpected(error{error_code::InvalidHuffmanTable, "Failed to decode temp symbol"});
                    }

                    if (code <= 2) {
                        // Skip codes
                        skip_code_ = static_cast<unsigned>(code);
                        state_ = state::READ_CODE_SKIP;
                        break;
                    } else {
                        tree_lengths_[tree_index_] = static_cast<u8>(code - 2);
                        tree_index_++;
                    }
                }
                if (tree_index_ >= tree_ncodes_ && state_ == state::READ_CODE_SYMBOL) {
                    state_ = state::BUILD_CODE_TREE;
                }
                break;
            }

            case state::READ_CODE_SKIP: {
                unsigned skip_count = 0;
                if (skip_code_ == 0) {
                    skip_count = 1;
                } else if (skip_code_ == 1) {
                    u32 extra = 0;
                    if (!try_read_bits(in_ptr, in_end, 4, extra)) {
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    skip_count = extra + 3;
                } else {
                    u32 extra = 0;
                    if (!try_read_bits(in_ptr, in_end, 9, extra)) {
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    skip_count = extra + 20;
                }
                for (unsigned i = 0; i < skip_count && tree_index_ < tree_ncodes_; i++) {
                    tree_lengths_[tree_index_] = 0;
                    tree_index_++;
                }
                state_ = state::READ_CODE_SYMBOL;
                break;
            }

            case state::BUILD_CODE_TREE: {
                if (!code_tree_.build(tree_lengths_.data(), tree_ncodes_)) {
                    return std::unexpected(error{error_code::InvalidHuffmanTable, "Failed to build code tree"});
                }
                state_ = state::READ_OFF_NCODES;
                break;
            }

            case state::READ_OFF_NCODES: {
                u32 n = 0;
                if (!try_read_bits(in_ptr, in_end, offset_bits_, n)) {
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                tree_ncodes_ = n;
                if (tree_ncodes_ == 0) {
                    state_ = state::READ_OFF_SINGLE;
                } else {
                    if (tree_ncodes_ > max_offset_codes_) tree_ncodes_ = static_cast<unsigned>(max_offset_codes_);
                    tree_index_ = 0;
                    tree_lengths_.fill(0);
                    in_length_ext_ = false;
                    state_ = state::READ_OFF_LENGTH;
                }
                break;
            }

            case state::READ_OFF_SINGLE: {
                u32 code = 0;
                if (!try_read_bits(in_ptr, in_end, offset_bits_, code)) {
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                offset_tree_.set_single(static_cast<u16>(code));
                state_ = state::DECODE_SYMBOL;
                break;
            }

            case state::READ_OFF_LENGTH: {
                while (tree_index_ < tree_ncodes_) {
                    if (!try_read_length_value(in_ptr, in_end)) {
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    tree_lengths_[tree_index_] = static_cast<u8>(code_length_value_);
                    tree_index_++;
                }
                state_ = state::BUILD_OFF_TREE;
                break;
            }

            case state::READ_OFF_LENGTH_EXT: {
                // Handled within try_read_length_value
                state_ = state::READ_OFF_LENGTH;
                break;
            }

            case state::BUILD_OFF_TREE: {
                if (!offset_tree_.build(tree_lengths_.data(), tree_ncodes_)) {
                    return std::unexpected(error{error_code::InvalidHuffmanTable, "Failed to build offset tree"});
                }
                state_ = state::DECODE_SYMBOL;
                break;
            }

            case state::DECODE_SYMBOL: {
                while (block_remaining_ > 0) {
                    if (out_ptr >= out_end) {
                        return stream_result::need_output(bytes_read(), bytes_written());
                    }

                    int code = code_tree_.read(reader);
                    if (code == -1) {
                        if (input_finished) {
                            state_ = state::DONE;
                            return stream_result::done(bytes_read(), bytes_written());
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    if (code < 0) {
                        return std::unexpected(error{error_code::CorruptData, "Failed to decode symbol"});
                    }

                    block_remaining_--;

                    if (code < 256) {
                        // Literal byte
                        u8 value = static_cast<u8>(code);
                        *out_ptr++ = value;
                        ringbuf_[ringbuf_pos_] = value;
                        ringbuf_pos_ = (ringbuf_pos_ + 1) % window_size_;
                    } else {
                        // Match: code 256+ represents length
                        copy_count_ = static_cast<size_t>(code - 256) + COPY_THRESHOLD;
                        state_ = state::READ_OFFSET;
                        break;
                    }
                }

                if (block_remaining_ == 0 && state_ == state::DECODE_SYMBOL) {
                    // Block complete, start next block
                    state_ = state::READ_BLOCK_SIZE;
                }
                break;
            }

            case state::READ_OFFSET: {
                int bits = offset_tree_.read(reader);
                if (bits == -1) {
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                if (bits < 0) {
                    return std::unexpected(error{error_code::CorruptData, "Failed to decode offset"});
                }

                if (bits == 0) {
                    copy_start_ = (ringbuf_pos_ + window_size_ - 1) % window_size_;
                    copy_pos_ = 0;
                    state_ = state::COPY_MATCH;
                } else if (bits == 1) {
                    copy_start_ = (ringbuf_pos_ + window_size_ - 2) % window_size_;
                    copy_pos_ = 0;
                    state_ = state::COPY_MATCH;
                } else {
                    offset_bits_value_ = static_cast<unsigned>(bits - 1);
                    state_ = state::READ_OFFSET_EXTRA;
                }
                break;
            }

            case state::READ_OFFSET_EXTRA: {
                u32 extra = 0;
                if (!try_read_bits(in_ptr, in_end, offset_bits_value_, extra)) {
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                size_t offset = (1u << offset_bits_value_) + extra;
                copy_start_ = (ringbuf_pos_ + window_size_ - offset - 1) % window_size_;
                copy_pos_ = 0;
                state_ = state::COPY_MATCH;
                break;
            }

            case state::COPY_MATCH: {
                while (copy_pos_ < copy_count_) {
                    if (out_ptr >= out_end) {
                        return stream_result::need_output(bytes_read(), bytes_written());
                    }
                    u8 value = ringbuf_[(copy_start_ + copy_pos_) % window_size_];
                    *out_ptr++ = value;
                    ringbuf_[ringbuf_pos_] = value;
                    ringbuf_pos_ = (ringbuf_pos_ + 1) % window_size_;
                    copy_pos_++;
                }
                state_ = state::DECODE_SYMBOL;
                break;
            }

            case state::DONE:
                break;
        }
    }

    return stream_result::done(bytes_read(), bytes_written());
}

}  // namespace crate
