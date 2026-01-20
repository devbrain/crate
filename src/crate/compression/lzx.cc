#include <crate/compression/lzx.hh>

namespace crate {

result_t<std::unique_ptr<lzx_decompressor>> lzx_decompressor::create(unsigned window_bits) {
    if (window_bits < lzx::MIN_WINDOW_BITS || window_bits > lzx::MAX_WINDOW_BITS) {
        return std::unexpected(error{error_code::InvalidParameter, "LZX window_bits must be 15-21"});
    }
    return std::make_unique<lzx_decompressor>(window_bits);
}
lzx_decompressor::lzx_decompressor(unsigned window_bits)
    : window_bits_(window_bits),
      window_size_(1u << window_bits),
      window_(window_size_, 0),
      num_position_slots_(calculate_position_slots(window_bits)) {
    init_state();
}
result_t<stream_result> lzx_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    const byte* in_ptr = input.data();
    const byte* in_end = input.data() + input.size();
    byte* out_ptr = output.data();
    if (!expected_output_set()) {
        return std::unexpected(error{
            error_code::InvalidParameter,
            "Expected size required for bounded decompression"
        });
    }
    auto bytes_read = [&]() -> size_t {
        return static_cast <size_t>(in_ptr - input.data());
    };
    auto bytes_written = [&]() -> size_t {
        return static_cast <size_t>(out_ptr - output.data());
    };
    auto finalize = [&](decode_status status) -> stream_result {
        size_t read = bytes_read();
        size_t written = bytes_written();
        advance_output(written);

        if (expected_output_set() && total_output_written() >= expected_output_size()) {
            state_ = state::DONE;
            return stream_result::done(read, written);
        }

        if (status == decode_status::needs_more_input) {
            return stream_result::need_input(read, written);
        }
        if (status == decode_status::needs_more_output) {
            return stream_result::need_output(read, written);
        }

        state_ = state::DONE;
        return stream_result::done(read, written);
    };

    size_t output_limit = output.size();
    if (expected_output_set()) {
        size_t written = total_output_written();
        if (written >= expected_output_size()) {
            state_ = state::DONE;
            return stream_result::done(bytes_read(), 0);
        }
        size_t remaining = expected_output_size() - written;
        if (remaining == 0) {
            state_ = state::DONE;
            return stream_result::done(bytes_read(), 0);
        }
        if (output_limit > remaining) {
            output_limit = remaining;
        }
    }
    byte* out_end = output.data() + output_limit;
    if (out_ptr >= out_end) {
        return stream_result::need_output(bytes_read(), bytes_written());
    }

    while (state_ != state::DONE) {
        switch (state_) {
            case state::READ_BLOCK_TYPE: {
                u32 value = 0;
                if (!try_read_bits(in_ptr, in_end, 3, value)) {
                    goto need_input;
                }
                block_type_ = static_cast <u8>(value);
                state_ = state::READ_BLOCK_SIZE_HI;
                break;
            }

            case state::READ_BLOCK_SIZE_HI: {
                u32 value = 0;
                if (!try_read_bits(in_ptr, in_end, 8, value)) {
                    goto need_input;
                }
                block_size_ = static_cast <size_t>(value) << 8;
                state_ = state::READ_BLOCK_SIZE_LO;
                break;
            }

            case state::READ_BLOCK_SIZE_LO: {
                u32 value = 0;
                if (!try_read_bits(in_ptr, in_end, 8, value)) {
                    goto need_input;
                }
                block_size_ |= static_cast <size_t>(value);
                if (block_size_ == 0) {
                    block_size_ = 32768;
                }
                block_remaining_ = block_size_;
                use_aligned_ = (block_type_ == lzx::BLOCKTYPE_ALIGNED);
                aligned_len_idx_ = 0;

                if (block_type_ == lzx::BLOCKTYPE_ALIGNED) {
                    state_ = state::READ_ALIGNED_TREE;
                } else if (block_type_ == lzx::BLOCKTYPE_VERBATIM) {
                    start_tree_reader(main_lengths_.data(), 0, lzx::NUM_CHARS);
                    state_ = state::READ_MAIN_TREE_0;
                } else if (block_type_ == lzx::BLOCKTYPE_UNCOMPRESSED) {
                    state_ = state::UNCOMPRESSED_ALIGN;
                } else {
                    return std::unexpected(error{error_code::InvalidBlockType, "Invalid LZX block type"});
                }
                break;
            }

            case state::READ_ALIGNED_TREE: {
                while (aligned_len_idx_ < lzx::NUM_ALIGNED_SYMBOLS) {
                    u32 len = 0;
                    if (!try_read_bits(in_ptr, in_end, 3, len)) {
                        goto need_input;
                    }
                    aligned_lengths_[aligned_len_idx_++] = static_cast <u8>(len);
                }
                auto result = aligned_decoder_.build(aligned_lengths_);
                if (!result) {
                    return std::unexpected(result.error());
                }
                start_tree_reader(main_lengths_.data(), 0, lzx::NUM_CHARS);
                state_ = state::READ_MAIN_TREE_0;
                break;
            }

            case state::READ_MAIN_TREE_0: {
                auto result = advance_tree_reader(in_ptr, in_end);
                if (!result) {
                    return std::unexpected(result.error());
                }
                if (!*result) {
                    goto need_input;
                }
                size_t main_tree_size = lzx::NUM_CHARS + num_position_slots_ * 8;
                start_tree_reader(main_lengths_.data(), lzx::NUM_CHARS, main_tree_size);
                state_ = state::READ_MAIN_TREE_1;
                break;
            }

            case state::READ_MAIN_TREE_1: {
                auto result = advance_tree_reader(in_ptr, in_end);
                if (!result) {
                    return std::unexpected(result.error());
                }
                if (!*result) {
                    goto need_input;
                }
                size_t main_tree_size = lzx::NUM_CHARS + num_position_slots_ * 8;
                auto build = main_decoder_.build(std::span(main_lengths_.data(), main_tree_size));
                if (!build) {
                    return std::unexpected(build.error());
                }
                start_tree_reader(length_lengths_.data(), 0, lzx::NUM_SECONDARY_LENGTHS);
                state_ = state::READ_LENGTH_TREE;
                break;
            }

            case state::READ_LENGTH_TREE: {
                auto result = advance_tree_reader(in_ptr, in_end);
                if (!result) {
                    return std::unexpected(result.error());
                }
                if (!*result) {
                    goto need_input;
                }
                auto build = length_decoder_.build(length_lengths_);
                if (!build) {
                    return std::unexpected(build.error());
                }
                state_ = state::DECODE_MAIN_SYMBOL;
                break;
            }

            case state::DECODE_MAIN_SYMBOL: {
                if (block_remaining_ == 0) {
                    state_ = state::READ_BLOCK_TYPE;
                    break;
                }

                if (out_ptr >= out_end) {
                    goto need_output;
                }

                auto decode = try_decode(main_decoder_, main_symbol_, in_ptr, in_end);
                if (!decode) {
                    return std::unexpected(decode.error());
                }
                if (!*decode) {
                    goto need_input;
                }

                if (main_symbol_ < lzx::NUM_CHARS) {
                    u8 value = static_cast <u8>(main_symbol_);
                    *out_ptr++ = value;
                    window_[window_pos_++ & (window_size_ - 1)] = value;
                    block_remaining_--;
                    break;
                }

                unsigned match_sym = main_symbol_ - lzx::NUM_CHARS;
                position_slot_ = match_sym / 8;
                length_header_ = match_sym % 8;

                if (length_header_ == 7) {
                    state_ = state::READ_LENGTH_SYMBOL;
                    break;
                }

                match_length_ = length_header_ + lzx::MIN_MATCH;

                if (position_slot_ == 0) {
                    match_offset_ = R0_;
                    match_remaining_ = match_length_;
                    state_ = state::COPY_MATCH;
                } else if (position_slot_ == 1) {
                    match_offset_ = R1_;
                    std::swap(R0_, R1_);
                    match_remaining_ = match_length_;
                    state_ = state::COPY_MATCH;
                } else if (position_slot_ == 2) {
                    match_offset_ = R2_;
                    std::swap(R0_, R2_);
                    match_remaining_ = match_length_;
                    state_ = state::COPY_MATCH;
                } else {
                    extra_bits_ = lzx::extra_bits[position_slot_];
                    verbatim_bits_ = 0;
                    aligned_bits_ = 0;
                    if (use_aligned_ && extra_bits_ >= 3) {
                        verbatim_bits_needed_ = extra_bits_ - 3;
                        state_ = state::READ_OFFSET_VERBATIM;
                    } else if (extra_bits_ > 0) {
                        verbatim_bits_needed_ = extra_bits_;
                        state_ = state::READ_OFFSET_VERBATIM;
                    } else {
                        match_offset_ = lzx::position_base[position_slot_];
                        R2_ = R1_;
                        R1_ = R0_;
                        R0_ = match_offset_;
                        match_remaining_ = match_length_;
                        state_ = state::COPY_MATCH;
                    }
                }
                break;
            }

            case state::READ_LENGTH_SYMBOL: {
                u16 len_sym = 0;
                auto decode = try_decode(length_decoder_, len_sym, in_ptr, in_end);
                if (!decode) {
                    return std::unexpected(decode.error());
                }
                if (!*decode) {
                    goto need_input;
                }
                match_length_ = len_sym + lzx::NUM_PRIMARY_LENGTHS + lzx::MIN_MATCH;

                if (position_slot_ == 0) {
                    match_offset_ = R0_;
                    match_remaining_ = match_length_;
                    state_ = state::COPY_MATCH;
                } else if (position_slot_ == 1) {
                    match_offset_ = R1_;
                    std::swap(R0_, R1_);
                    match_remaining_ = match_length_;
                    state_ = state::COPY_MATCH;
                } else if (position_slot_ == 2) {
                    match_offset_ = R2_;
                    std::swap(R0_, R2_);
                    match_remaining_ = match_length_;
                    state_ = state::COPY_MATCH;
                } else {
                    extra_bits_ = lzx::extra_bits[position_slot_];
                    verbatim_bits_ = 0;
                    aligned_bits_ = 0;
                    if (use_aligned_ && extra_bits_ >= 3) {
                        verbatim_bits_needed_ = extra_bits_ - 3;
                        state_ = state::READ_OFFSET_VERBATIM;
                    } else if (extra_bits_ > 0) {
                        verbatim_bits_needed_ = extra_bits_;
                        state_ = state::READ_OFFSET_VERBATIM;
                    } else {
                        match_offset_ = lzx::position_base[position_slot_];
                        R2_ = R1_;
                        R1_ = R0_;
                        R0_ = match_offset_;
                        match_remaining_ = match_length_;
                        state_ = state::COPY_MATCH;
                    }
                }
                break;
            }

            case state::READ_OFFSET_VERBATIM: {
                if (verbatim_bits_needed_ > 0) {
                    u32 bits = 0;
                    if (!try_read_bits(in_ptr, in_end, verbatim_bits_needed_, bits)) {
                        goto need_input;
                    }
                    verbatim_bits_ = bits;
                }

                if (use_aligned_ && extra_bits_ >= 3) {
                    state_ = state::READ_OFFSET_ALIGNED;
                    break;
                }

                match_offset_ = lzx::position_base[position_slot_] + verbatim_bits_;
                R2_ = R1_;
                R1_ = R0_;
                R0_ = match_offset_;
                match_remaining_ = match_length_;
                state_ = state::COPY_MATCH;
                break;
            }

            case state::READ_OFFSET_ALIGNED: {
                u16 aligned_sym = 0;
                auto decode = try_decode(aligned_decoder_, aligned_sym, in_ptr, in_end);
                if (!decode) {
                    return std::unexpected(decode.error());
                }
                if (!*decode) {
                    goto need_input;
                }
                aligned_bits_ = aligned_sym;
                match_offset_ = lzx::position_base[position_slot_] + (verbatim_bits_ << 3) + aligned_bits_;
                R2_ = R1_;
                R1_ = R0_;
                R0_ = match_offset_;
                match_remaining_ = match_length_;
                state_ = state::COPY_MATCH;
                break;
            }

            case state::COPY_MATCH: {
                while (match_remaining_ > 0 && block_remaining_ > 0) {
                    if (out_ptr >= out_end) {
                        goto need_output;
                    }
                    u8 value = window_[(window_pos_ - match_offset_) & (window_size_ - 1)];
                    *out_ptr++ = value;
                    window_[window_pos_++ & (window_size_ - 1)] = value;
                    match_remaining_--;
                    block_remaining_--;
                }

                if (block_remaining_ == 0) {
                    match_remaining_ = 0;
                    state_ = state::READ_BLOCK_TYPE;
                } else if (match_remaining_ == 0) {
                    state_ = state::DECODE_MAIN_SYMBOL;
                }
                break;
            }

            case state::UNCOMPRESSED_ALIGN:
                align_to_byte();
                uncompressed_value_ = 0;
                uncompressed_bytes_read_ = 0;
                state_ = state::UNCOMPRESSED_R0;
                break;

            case state::UNCOMPRESSED_R0:
                if (!try_read_u32_le(in_ptr, in_end, R0_)) {
                    goto need_input;
                }
                state_ = state::UNCOMPRESSED_R1;
                break;

            case state::UNCOMPRESSED_R1:
                if (!try_read_u32_le(in_ptr, in_end, R1_)) {
                    goto need_input;
                }
                state_ = state::UNCOMPRESSED_R2;
                break;

            case state::UNCOMPRESSED_R2:
                if (!try_read_u32_le(in_ptr, in_end, R2_)) {
                    goto need_input;
                }
                state_ = state::UNCOMPRESSED_COPY;
                break;

            case state::UNCOMPRESSED_COPY:
                while (block_remaining_ > 0) {
                    if (out_ptr >= out_end) {
                        goto need_output;
                    }
                    u8 value = 0;
                    if (!try_read_byte(in_ptr, in_end, value)) {
                        goto need_input;
                    }
                    *out_ptr++ = value;
                    window_[window_pos_++ & (window_size_ - 1)] = value;
                    block_remaining_--;
                }

                if (block_remaining_ == 0) {
                    if (block_size_ & 1) {
                        state_ = state::UNCOMPRESSED_PAD;
                    } else {
                        state_ = state::READ_BLOCK_TYPE;
                    }
                }
                break;

            case state::UNCOMPRESSED_PAD: {
                u8 pad = 0;
                if (!try_read_byte(in_ptr, in_end, pad)) {
                    goto need_input;
                }
                state_ = state::READ_BLOCK_TYPE;
                break;
            }

            case state::DONE:
                break;
        }
    }

    return finalize(decode_status::done);

need_input:
    if (input_finished) {
        return std::unexpected(error{error_code::InputBufferUnderflow});
    }
    return finalize(decode_status::needs_more_input);

need_output:
    return finalize(decode_status::needs_more_output);
}
void lzx_decompressor::reset() {
    init_state();
}
void lzx_decompressor::init_state() {
    clear_expected_output_size();
    R0_ = 1;
    R1_ = 1;
    R2_ = 1;
    window_pos_ = 0;
    std::fill(window_.begin(), window_.end(), 0);
    std::fill(main_lengths_.begin(), main_lengths_.end(), 0);
    std::fill(length_lengths_.begin(), length_lengths_.end(), 0);
    state_ = state::READ_BLOCK_TYPE;
    bit_buffer_ = 0;
    bits_left_ = 0;
    block_type_ = 0;
    block_size_ = 0;
    block_remaining_ = 0;
    use_aligned_ = false;
    aligned_len_idx_ = 0;
    std::fill(aligned_lengths_.begin(), aligned_lengths_.end(), 0);
    tree_state_ = tree_state::READ_PRETREE_LENGTHS;
    run_state_ = run_state::NONE;
    std::fill(pretree_lengths_.begin(), pretree_lengths_.end(), 0);
    pretree_len_idx_ = 0;
    lengths_ptr_ = nullptr;
    lengths_idx_ = 0;
    lengths_end_ = 0;
    run_symbol_ = 0;
    run_bits_ = 0;
    run_base_ = 0;
    run_remaining_ = 0;
    run_value_ = 0;
    main_symbol_ = 0;
    position_slot_ = 0;
    length_header_ = 0;
    match_length_ = 0;
    match_offset_ = 0;
    match_remaining_ = 0;
    extra_bits_ = 0;
    verbatim_bits_needed_ = 0;
    verbatim_bits_ = 0;
    aligned_bits_ = 0;
    uncompressed_value_ = 0;
    uncompressed_bytes_read_ = 0;
}
unsigned lzx_decompressor::calculate_position_slots(unsigned window_bits) {
    if (window_bits < 15)
        return 2 * window_bits;
    if (window_bits < 17)
        return 32 + 2 * (window_bits - 15);
    return 36 + 2 * (window_bits - 17);
}
bool lzx_decompressor::try_peek_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
    if (n == 0) {
        out = 0;
        return true;
    }

    while (bits_left_ < n) {
        if (ptr >= end) {
            return false;
        }
        bit_buffer_ = (bit_buffer_ << 8) | *ptr++;
        bits_left_ += 8;
    }

    out = static_cast <u32>((bit_buffer_ >> (bits_left_ - n)) & ((1ULL << n) - 1));
    return true;
}

bool lzx_decompressor::try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
    u32 value = 0;
    if (!try_peek_bits(ptr, end, n, value)) {
        return false;
    }
    remove_bits(n);
    out = value;
    return true;
}

bool lzx_decompressor::try_read_byte(const byte*& ptr, const byte* end, u8& out) {
    u32 value = 0;
    if (!try_read_bits(ptr, end, 8, value)) {
        return false;
    }
    out = static_cast <u8>(value);
    return true;
}

bool lzx_decompressor::try_read_u32_le(const byte*& ptr, const byte* end, u32& out) {
    while (uncompressed_bytes_read_ < 4) {
        u8 next_byte = 0;
        if (!try_read_byte(ptr, end, next_byte)) {
            return false;
        }
        uncompressed_value_ |= static_cast <u32>(next_byte) << (8 * uncompressed_bytes_read_);
        uncompressed_bytes_read_++;
    }

    out = uncompressed_value_;
    uncompressed_value_ = 0;
    uncompressed_bytes_read_ = 0;
    return true;
}

void lzx_decompressor::remove_bits(unsigned n) {
    bits_left_ -= n;
}

void lzx_decompressor::align_to_byte() {
    bits_left_ -= bits_left_ % 8;
}

template<size_t N>
result_t <bool> lzx_decompressor::try_decode(
    huffman_decoder<N>& decoder,
    u16& out,
    const byte*& ptr,
    const byte* end
) {
    struct msb_reader {
        lzx_decompressor& owner;
        const byte*& ptr;
        const byte* end;

        bool try_peek_bits(unsigned n, u32& out) {
            return owner.try_peek_bits(ptr, end, n, out);
        }

        void remove_bits(unsigned n) {
            owner.remove_bits(n);
        }
    };

    msb_reader reader{*this, ptr, end};
    return decoder.try_decode_msb(reader, out);
}

void lzx_decompressor::start_tree_reader(u8* lengths, size_t start, size_t end) {
    lengths_ptr_ = lengths;
    lengths_idx_ = start;
    lengths_end_ = end;
    pretree_len_idx_ = 0;
    tree_state_ = tree_state::READ_PRETREE_LENGTHS;
    run_state_ = run_state::NONE;
    run_remaining_ = 0;
}

result_t <bool> lzx_decompressor::advance_tree_reader(const byte*& ptr, const byte* end) {
    while (true) {
        switch (tree_state_) {
            case tree_state::READ_PRETREE_LENGTHS: {
                while (pretree_len_idx_ < pretree_lengths_.size()) {
                    u32 len = 0;
                    if (!try_read_bits(ptr, end, 4, len)) {
                        return false;
                    }
                    pretree_lengths_[pretree_len_idx_++] = static_cast <u8>(len);
                }
                tree_state_ = tree_state::BUILD_PRETREE;
                break;
            }

            case tree_state::BUILD_PRETREE: {
                auto result = pretree_decoder_.build(pretree_lengths_);
                if (!result) {
                    return std::unexpected(result.error());
                }
                tree_state_ = tree_state::DECODE_LENGTHS;
                break;
            }

            case tree_state::DECODE_LENGTHS: {
                while (lengths_idx_ < lengths_end_) {
                    if (run_state_ == run_state::FILL_RUN) {
                        while (run_remaining_ > 0 && lengths_idx_ < lengths_end_) {
                            lengths_ptr_[lengths_idx_++] = run_value_;
                            run_remaining_--;
                        }
                        if (run_remaining_ == 0) {
                            run_state_ = run_state::NONE;
                        }
                        continue;
                    }

                    if (run_state_ == run_state::READ_RUN_BITS) {
                        u32 extra = 0;
                        if (!try_read_bits(ptr, end, run_bits_, extra)) {
                            return false;
                        }
                        run_remaining_ = run_base_ + extra;
                        if (run_symbol_ == 19) {
                            run_state_ = run_state::READ_REPEAT_SYMBOL;
                        } else {
                            run_value_ = 0;
                            run_state_ = run_state::FILL_RUN;
                        }
                        continue;
                    }

                    if (run_state_ == run_state::READ_REPEAT_SYMBOL) {
                        u16 repeat = 0;
                        auto decode = try_decode(pretree_decoder_, repeat, ptr, end);
                        if (!decode) {
                            return std::unexpected(decode.error());
                        }
                        if (!*decode) {
                            return false;
                        }
                        run_value_ = static_cast <u8>((lengths_ptr_[lengths_idx_] + 17 - repeat) % 17);
                        run_state_ = run_state::FILL_RUN;
                        continue;
                    }

                    u16 sym = 0;
                    auto decode = try_decode(pretree_decoder_, sym, ptr, end);
                    if (!decode) {
                        return std::unexpected(decode.error());
                    }
                    if (!*decode) {
                        return false;
                    }

                    if (sym < 17) {
                        lengths_ptr_[lengths_idx_] =
                            static_cast <u8>((lengths_ptr_[lengths_idx_] + 17 - sym) % 17);
                        lengths_idx_++;
                    } else if (sym == 17) {
                        run_symbol_ = 17;
                        run_bits_ = 4;
                        run_base_ = 4;
                        run_state_ = run_state::READ_RUN_BITS;
                    } else if (sym == 18) {
                        run_symbol_ = 18;
                        run_bits_ = 5;
                        run_base_ = 20;
                        run_state_ = run_state::READ_RUN_BITS;
                    } else {
                        run_symbol_ = 19;
                        run_bits_ = 1;
                        run_base_ = 4;
                        run_state_ = run_state::READ_RUN_BITS;
                    }
                }

                tree_state_ = tree_state::DONE;
                return true;
            }

            case tree_state::DONE:
                return true;
        }
    }
}
}  // namespace crate
