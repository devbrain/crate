#include <crate/compression/lzh.hh>

namespace crate {

void lzh_huffman_tree::clear() {
    codes_.clear();
    lengths_.clear();
    lookup_.fill(0xFFFF);
    max_length_ = 0;
    is_single_ = false;
}

bool lzh_huffman_tree::build_from_lengths(const std::vector<unsigned>& lengths) {
    clear();
    if (lengths.empty())
        return false;

    unsigned num_codes = static_cast<unsigned>(lengths.size());
    lengths_.resize(num_codes);
    codes_.resize(num_codes);

    // Find max length and count codes per length
    std::array<unsigned, MAX_CODE_LENGTH + 1> length_count{};
    max_length_ = 0;
    for (unsigned i = 0; i < num_codes; i++) {
        lengths_[i] = lengths[i];
        if (lengths[i] > 0) {
            if (lengths[i] > MAX_CODE_LENGTH)
                return false;
            length_count[lengths[i]]++;
            max_length_ = std::max(max_length_, lengths[i]);
        }
    }

    if (max_length_ == 0)
        return false;

    // Generate canonical codes
    std::array<unsigned, MAX_CODE_LENGTH + 1> next_code{};
    unsigned code = 0;
    for (unsigned bits = 1; bits <= max_length_; bits++) {
        code = (code + length_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    // Assign codes to symbols
    for (unsigned i = 0; i < num_codes; i++) {
        if (lengths_[i] > 0) {
            codes_[i] = next_code[lengths_[i]]++;
        }
    }

    // Build lookup table for fast decoding (first 8 bits)
    build_lookup_table();

    return true;
}

void lzh_huffman_tree::build_single(unsigned value) {
    clear();
    lengths_.resize(value + 1, 0);
    codes_.resize(value + 1, 0);
    lengths_[value] = 1;
    codes_[value] = 0;
    max_length_ = 1;
    single_value_ = value;
    is_single_ = true;
}

void lzh_huffman_tree::build_lookup_table() {
    lookup_.fill(0xFFFF);

    for (unsigned i = 0; i < codes_.size(); i++) {
        if (lengths_[i] > 0 && lengths_[i] <= 8) {
            // Fill all entries that match this code
            unsigned code = codes_[i];
            unsigned len = lengths_[i];
            unsigned shift = 8 - len;
            unsigned base = code << shift;
            unsigned count = 1u << shift;

            for (unsigned j = 0; j < count; j++) {
                unsigned entry = (len << 12) | i;
                lookup_[base + j] = static_cast<u16>(entry);
            }
        }
    }
}

lzh_decompressor::lzh_decompressor(lzh_format format) : format_(format) {
    switch (format) {
        case lzh_format::LH5:
            window_size_ = 8192;
            offsets_bits_ = 4;
            max_offset_codes_ = 14;
            break;
        case lzh_format::LH6:
            window_size_ = 32768;
            offsets_bits_ = 5;
            max_offset_codes_ = 16;
            break;
        case lzh_format::LH7:
            window_size_ = 65536;
            offsets_bits_ = 5;
            max_offset_codes_ = 17;
            break;
    }
    init_state();
}

lzh_decompressor::lzh_decompressor(size_t window_size, unsigned offset_bits, unsigned max_offset_codes)
    : format_(lzh_format::LH5),  // Default, not actually used
      window_size_(window_size),
      offsets_bits_(offset_bits),
      max_offset_codes_(max_offset_codes) {
    init_state();
}

void lzh_decompressor::reset() {
    init_state();
}

void lzh_decompressor::init_state() {
    window_.resize(window_size_);
    std::fill(window_.begin(), window_.end(), 0);
    window_pos_ = 0;

    bit_buffer_ = 0;
    bits_left_ = 0;

    state_ = state::READ_BLOCK_SIZE;
    block_codes_total_ = 0;
    block_codes_done_ = 0;

    tree_ncodes_ = 0;
    tree_index_ = 0;
    tree_lengths_.clear();

    code_length_value_ = 0;
    skip_code_ = 0;

    match_length_ = 0;
    match_offset_ = 0;
    match_remaining_ = 0;
    offset_code_ = 0;
    extra_bits_needed_ = 0;
}

bool lzh_decompressor::try_peek_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
    if (n == 0) {
        out = 0;
        return true;
    }

    // Fill bit buffer with bytes (MSB first)
    while (bits_left_ < n) {
        if (ptr >= end) {
            return false;
        }
        bit_buffer_ = (bit_buffer_ << 8) | *ptr++;
        bits_left_ += 8;
    }

    out = static_cast<u32>((bit_buffer_ >> (bits_left_ - n)) & ((1ULL << n) - 1));
    return true;
}

bool lzh_decompressor::try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
    if (!try_peek_bits(ptr, end, n, out)) {
        return false;
    }
    remove_bits(n);
    return true;
}

void lzh_decompressor::remove_bits(unsigned n) {
    bits_left_ -= n;
}

result_t<stream_result> lzh_decompressor::decompress_some(
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

    // Bit reader struct for Huffman decoding
    struct bit_reader {
        lzh_decompressor& owner;
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
                u32 value = 0;
                if (!try_read_bits(in_ptr, in_end, 16, value)) {
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
                block_codes_total_ = value;
                if (block_codes_total_ == 0) {
                    // Block size 0 can mean:
                    // 1. End of stream (if we've written output)
                    // 2. 65536 codes (ARJ extension, only for ARJ method 1-3)
                    // For safety, treat 0 as end-of-stream if we have output
                    if (bytes_written() > 0) {
                        state_ = state::DONE;
                        return stream_result::done(bytes_read(), bytes_written());
                    }
                    // Otherwise, interpret as 65536 codes (ARJ extension)
                    block_codes_total_ = 65536;
                }
                block_codes_done_ = 0;
                state_ = state::READ_CL_TREE_NCODES;
                break;
            }

            case state::READ_CL_TREE_NCODES: {
                u32 value = 0;
                if (!try_read_bits(in_ptr, in_end, 5, value)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                tree_ncodes_ = value;
                if (tree_ncodes_ > 20) {
                    tree_ncodes_ = 20;
                }
                if (tree_ncodes_ == 0) {
                    state_ = state::READ_CL_TREE_SINGLE;
                } else {
                    tree_lengths_.assign(tree_ncodes_, 0);
                    tree_index_ = 0;
                    state_ = state::READ_CL_TREE_LENGTH;
                }
                break;
            }

            case state::READ_CL_TREE_SINGLE: {
                u32 value = 0;
                if (!try_read_bits(in_ptr, in_end, 5, value)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                codelengths_tree_.build_single(value);
                state_ = state::READ_LIT_TREE_NCODES;
                break;
            }

            case state::READ_CL_TREE_LENGTH: {
                // Read code length: 3 bits, if 7 then extend with 1-bits
                while (tree_index_ < tree_ncodes_) {
                    u32 len = 0;
                    if (!try_read_bits(in_ptr, in_end, 3, len)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }

                    if (len == 7) {
                        // Need to read extension bits
                        code_length_value_ = 7;
                        state_ = state::READ_CL_TREE_LENGTH_EXT;
                        break;
                    }

                    tree_lengths_[tree_index_] = len;
                    tree_index_++;

                    // After first 3 lengths (indices 0,1,2), read optional skip
                    if (tree_index_ == 3 && tree_index_ < tree_ncodes_) {
                        state_ = state::READ_CL_TREE_SKIP;
                        break;
                    }
                }

                if (tree_index_ >= tree_ncodes_ && state_ == state::READ_CL_TREE_LENGTH) {
                    state_ = state::BUILD_CL_TREE;
                }
                break;
            }

            case state::READ_CL_TREE_LENGTH_EXT: {
                // Continue reading extension bits for code length
                while (true) {
                    u32 bit = 0;
                    if (!try_read_bits(in_ptr, in_end, 1, bit)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    if (bit == 0) {
                        break;
                    }
                    code_length_value_++;
                    if (code_length_value_ > 16) {
                        return std::unexpected(error{error_code::InvalidHuffmanTable, "Code length too large"});
                    }
                }

                tree_lengths_[tree_index_] = code_length_value_;
                tree_index_++;

                // After first 3 lengths (indices 0,1,2), read optional skip
                if (tree_index_ == 3 && tree_index_ < tree_ncodes_) {
                    state_ = state::READ_CL_TREE_SKIP;
                } else if (tree_index_ >= tree_ncodes_) {
                    state_ = state::BUILD_CL_TREE;
                } else {
                    state_ = state::READ_CL_TREE_LENGTH;
                }
                break;
            }

            case state::READ_CL_TREE_SKIP: {
                u32 skip = 0;
                if (!try_read_bits(in_ptr, in_end, 2, skip)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                tree_index_ += skip;
                state_ = state::READ_CL_TREE_LENGTH;
                break;
            }

            case state::BUILD_CL_TREE: {
                if (!codelengths_tree_.build_from_lengths(tree_lengths_)) {
                    return std::unexpected(error{error_code::InvalidHuffmanTable, "Failed to build code lengths tree"});
                }
                state_ = state::READ_LIT_TREE_NCODES;
                break;
            }

            case state::READ_LIT_TREE_NCODES: {
                u32 value = 0;
                if (!try_read_bits(in_ptr, in_end, 9, value)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                tree_ncodes_ = value;
                if (tree_ncodes_ > 510) {
                    return std::unexpected(error{error_code::InvalidHuffmanTable, "Too many literal codes"});
                }
                if (tree_ncodes_ == 0) {
                    state_ = state::READ_LIT_TREE_SINGLE;
                } else {
                    tree_lengths_.assign(tree_ncodes_, 0);
                    tree_index_ = 0;
                    state_ = state::READ_LIT_TREE_CODE;
                }
                break;
            }

            case state::READ_LIT_TREE_SINGLE: {
                u32 value = 0;
                if (!try_read_bits(in_ptr, in_end, 9, value)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                if (value >= 510) {
                    return std::unexpected(error{error_code::InvalidHuffmanTable});
                }
                literals_tree_.build_single(value);
                state_ = state::READ_OFF_TREE_NCODES;
                break;
            }

            case state::READ_LIT_TREE_CODE: {
                while (tree_index_ < tree_ncodes_) {
                    u16 code = 0;
                    auto decode_result = codelengths_tree_.try_decode(reader, code);
                    if (!decode_result) {
                        return std::unexpected(decode_result.error());
                    }
                    if (!*decode_result) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }

                    if (code <= 2) {
                        // Skip codes: 0=skip 1, 1=skip 3+(0-15), 2=skip 20+(0-511)
                        skip_code_ = code;
                        state_ = state::READ_LIT_TREE_SKIP;
                        break;
                    } else {
                        tree_lengths_[tree_index_] = code - 2;
                        tree_index_++;
                    }
                }

                if (tree_index_ >= tree_ncodes_ && state_ == state::READ_LIT_TREE_CODE) {
                    state_ = state::BUILD_LIT_TREE;
                }
                break;
            }

            case state::READ_LIT_TREE_SKIP: {
                unsigned skip = 0;
                if (skip_code_ == 0) {
                    skip = 0;
                } else if (skip_code_ == 1) {
                    u32 extra = 0;
                    if (!try_read_bits(in_ptr, in_end, 4, extra)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    skip = 2 + extra;
                } else {
                    u32 extra = 0;
                    if (!try_read_bits(in_ptr, in_end, 9, extra)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    skip = 19 + extra;
                }
                tree_index_ += 1 + skip;
                state_ = state::READ_LIT_TREE_CODE;
                break;
            }

            case state::BUILD_LIT_TREE: {
                if (!literals_tree_.build_from_lengths(tree_lengths_)) {
                    return std::unexpected(error{error_code::InvalidHuffmanTable, "Failed to build literals tree"});
                }
                state_ = state::READ_OFF_TREE_NCODES;
                break;
            }

            case state::READ_OFF_TREE_NCODES: {
                u32 value = 0;
                if (!try_read_bits(in_ptr, in_end, offsets_bits_, value)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                tree_ncodes_ = value;
                if (tree_ncodes_ > max_offset_codes_) {
                    return std::unexpected(error{error_code::InvalidHuffmanTable, "Too many offset codes"});
                }
                if (tree_ncodes_ == 0) {
                    state_ = state::READ_OFF_TREE_SINGLE;
                } else {
                    tree_lengths_.assign(tree_ncodes_, 0);
                    tree_index_ = 0;
                    state_ = state::READ_OFF_TREE_LENGTH;
                }
                break;
            }

            case state::READ_OFF_TREE_SINGLE: {
                u32 value = 0;
                if (!try_read_bits(in_ptr, in_end, offsets_bits_, value)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                if (value >= max_offset_codes_) {
                    return std::unexpected(error{error_code::InvalidHuffmanTable});
                }
                offsets_tree_.build_single(value);
                state_ = state::DECODE_SYMBOL;
                break;
            }

            case state::READ_OFF_TREE_LENGTH: {
                while (tree_index_ < tree_ncodes_) {
                    u32 len = 0;
                    if (!try_read_bits(in_ptr, in_end, 3, len)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }

                    if (len == 7) {
                        // Need to read extension bits
                        code_length_value_ = 7;
                        state_ = state::READ_OFF_TREE_LENGTH_EXT;
                        break;
                    }

                    tree_lengths_[tree_index_] = len;
                    tree_index_++;
                }

                if (tree_index_ >= tree_ncodes_ && state_ == state::READ_OFF_TREE_LENGTH) {
                    state_ = state::BUILD_OFF_TREE;
                }
                break;
            }

            case state::READ_OFF_TREE_LENGTH_EXT: {
                // Continue reading extension bits for offset code length
                while (true) {
                    u32 bit = 0;
                    if (!try_read_bits(in_ptr, in_end, 1, bit)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    if (bit == 0) {
                        break;
                    }
                    code_length_value_++;
                    if (code_length_value_ > 16) {
                        return std::unexpected(error{error_code::InvalidHuffmanTable, "Code length too large"});
                    }
                }

                tree_lengths_[tree_index_] = code_length_value_;
                tree_index_++;

                if (tree_index_ >= tree_ncodes_) {
                    state_ = state::BUILD_OFF_TREE;
                } else {
                    state_ = state::READ_OFF_TREE_LENGTH;
                }
                break;
            }

            case state::BUILD_OFF_TREE: {
                if (!offsets_tree_.build_from_lengths(tree_lengths_)) {
                    return std::unexpected(error{error_code::InvalidHuffmanTable, "Failed to build offsets tree"});
                }
                state_ = state::DECODE_SYMBOL;
                break;
            }

            case state::DECODE_SYMBOL: {
                while (block_codes_done_ < block_codes_total_) {
                    if (out_ptr >= out_end) {
                        return stream_result::need_output(bytes_read(), bytes_written());
                    }

                    u16 code = 0;
                    auto decode_result = literals_tree_.try_decode(reader, code);
                    if (!decode_result) {
                        return std::unexpected(decode_result.error());
                    }
                    if (!*decode_result) {
                        if (input_finished) {
                            // Graceful end - might have trailing bits
                            state_ = state::DONE;
                            return stream_result::done(bytes_read(), bytes_written());
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }

                    if (code < 256) {
                        // Literal byte
                        u8 value = static_cast<u8>(code);
                        *out_ptr++ = value;
                        window_[window_pos_++ % window_size_] = value;
                        block_codes_done_++;
                    } else {
                        // Match: code 256-509 represents length 3-256
                        match_length_ = code - 253;
                        state_ = state::DECODE_OFFSET;
                        break;
                    }
                }

                if (block_codes_done_ >= block_codes_total_ && state_ == state::DECODE_SYMBOL) {
                    // Block complete - if output buffer is full, we're done
                    if (out_ptr >= out_end) {
                        state_ = state::DONE;
                        return stream_result::done(bytes_read(), bytes_written());
                    }

                    // Start next block
                    state_ = state::READ_BLOCK_SIZE;
                }
                break;
            }

            case state::DECODE_OFFSET: {
                u16 ocode = 0;
                auto decode_result = offsets_tree_.try_decode(reader, ocode);
                if (!decode_result) {
                    return std::unexpected(decode_result.error());
                }
                if (!*decode_result) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                offset_code_ = ocode;
                if (offset_code_ <= 1) {
                    match_offset_ = offset_code_;
                    match_remaining_ = match_length_;
                    state_ = state::COPY_MATCH;
                } else {
                    extra_bits_needed_ = offset_code_ - 1;
                    state_ = state::READ_EXTRA_BITS;
                }
                break;
            }

            case state::READ_EXTRA_BITS: {
                u32 extra = 0;
                if (!try_read_bits(in_ptr, in_end, extra_bits_needed_, extra)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                match_offset_ = (1u << extra_bits_needed_) + extra;
                match_remaining_ = match_length_;
                state_ = state::COPY_MATCH;
                break;
            }

            case state::COPY_MATCH: {
                while (match_remaining_ > 0) {
                    if (out_ptr >= out_end) {
                        return stream_result::need_output(bytes_read(), bytes_written());
                    }
                    size_t src_pos = (window_pos_ - match_offset_ - 1 + window_size_) % window_size_;
                    u8 value = window_[src_pos];
                    *out_ptr++ = value;
                    window_[window_pos_++ % window_size_] = value;
                    match_remaining_--;
                }
                block_codes_done_++;
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
