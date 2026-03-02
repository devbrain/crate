#include <crate/compression/rar_15.hh>

namespace crate {

void rar_15_decompressor::init_state() {
    // Initialize window
    if (window_.size() < WINDOW_SIZE) {
        window_.resize(WINDOW_SIZE);
    }
    std::fill(window_.begin(), window_.end(), u8(0));

    unp_ptr_ = 0;
    for (auto& d : old_dist_) d = 0;
    old_dist_ptr_ = 0;
    last_dist_ = 0;
    last_length_ = 0;

    bit_buffer_ = 0;
    bits_left_ = 0;

    state_ = state::READ_FLAG_BYTE;

    init_huff();
    stmode_ = false;
    lcount_ = 0;
    flags_cnt_ = 0;
    flag_buf_ = 0;
    avr_ln1_ = 0;
    avr_ln2_ = 0;
    avr_ln3_ = 0;
    avr_plc_ = 0;
    avr_plc_b_ = 0;
    num_huf_ = 0;
    nhfb_ = 0;
    max_dist3_ = 0x2001;

    cur_length_ = 0;
    cur_distance_ = 0;
    cur_slot_ = 0;
    cur_pos_ = 0;
    cur_hf_idx_ = 0;
    match_remaining_ = 0;
    initialized_ = true;
}

void rar_15_decompressor::init_huff() {
    // Initialize character sets with initial priority values
    for (unsigned i = 0; i < 256; i++) {
        ch_set_[i] = static_cast<u16>(i << 8);
        ch_set_b_[i] = static_cast<u16>(i << 8);
    }

    // Initialize place tables
    for (unsigned i = 0; i < 256; i++) {
        place_[i] = place_b_[i] = place_c_[i] = 0;
    }

    // Initialize position counters
    for (unsigned i = 0; i < 16; i++) {
        ntopl_[i] = 0;
        ntopl_b_[i] = 0;
        ntopl_c_[i] = 0;
    }

    // Set up initial decode tables
    corr_huff(ch_set_, ntopl_);
    corr_huff(ch_set_b_, ntopl_b_);
}

void rar_15_decompressor::corr_huff(u16* ch_set, unsigned* ntopl) {
    // Reinitialize character set with priority distribution
    for (unsigned i = 7; i > 0; i--) {
        for (unsigned j = 0; j < 32; j++) {
            unsigned pos = (7 - i) * 32 + j;
            if (pos < 256) {
                ch_set[pos] = static_cast<u16>((ch_set[pos] & 0xFF00) |
                                               static_cast<u16>(i));
            }
        }
    }
    // Clear position counters
    for (unsigned i = 0; i < 16; i++) {
        ntopl[i] = 0;
    }
}

bool rar_15_decompressor::try_peek_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
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

bool rar_15_decompressor::try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
    if (!try_peek_bits(ptr, end, n, out)) {
        return false;
    }
    remove_bits(n);
    return true;
}

void rar_15_decompressor::remove_bits(unsigned n) {
    bits_left_ -= n;
}

void rar_15_decompressor::insert_old_dist(unsigned distance) {
    for (int i = 3; i > 0; i--) {
        old_dist_[i] = old_dist_[i - 1];
    }
    old_dist_[0] = distance;
}

result_t<stream_result> rar_15_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    if (!initialized_) {
        init_state();
    }

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

    while (state_ != state::DONE) {
        switch (state_) {
            case state::READ_FLAG_BYTE: {
                if (flags_cnt_ == 0) {
                    u32 bits = 0;
                    if (!try_read_bits(in_ptr, in_end, 8, bits)) {
                        if (input_finished) {
                            if (bytes_written() > 0) {
                                state_ = state::DONE;
                                return stream_result::done(bytes_read(), bytes_written());
                            }
                            state_ = state::DONE;
                            return stream_result::done(bytes_read(), bytes_written());
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    flag_buf_ = bits;
                    flags_cnt_ = 8;
                }
                state_ = state::CHECK_FLAG1;
                break;
            }

            case state::CHECK_FLAG1: {
                flags_cnt_--;
                if ((flag_buf_ & 0x80) != 0) {
                    flag_buf_ <<= 1;
                    state_ = state::CHECK_FLAG2;
                } else {
                    flag_buf_ <<= 1;
                    state_ = state::HUFF_DECODE;
                }
                break;
            }

            case state::CHECK_FLAG2: {
                // May need to read more flag bits
                if (flags_cnt_ == 0) {
                    u32 bits = 0;
                    if (!try_read_bits(in_ptr, in_end, 8, bits)) {
                        if (input_finished) {
                            state_ = state::DONE;
                            return stream_result::done(bytes_read(), bytes_written());
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    flag_buf_ = bits;
                    flags_cnt_ = 8;
                }
                flags_cnt_--;

                if ((flag_buf_ & 0x80) != 0) {
                    flag_buf_ <<= 1;
                    state_ = state::LONG_LZ_READ_LENGTH;
                } else {
                    flag_buf_ <<= 1;
                    state_ = state::SHORT_LZ_CHECK_REPEAT;
                }
                break;
            }

            case state::HUFF_DECODE: {
                // Peek bits for Huffman decode
                u32 bits = 0;
                if (!try_peek_bits(in_ptr, in_end, 16, bits)) {
                    if (input_finished) {
                        state_ = state::DONE;
                        return stream_result::done(bytes_read(), bytes_written());
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                // Find matching Huffman entry
                cur_hf_idx_ = 0;
                for (unsigned i = 0; i < 8; i++) {
                    if (bits < hf_dec_[i]) {
                        break;
                    }
                    cur_hf_idx_ = i + 1;
                }

                unsigned hf_bits = cur_hf_idx_ < 2 ? 1 : (cur_hf_idx_ < 4 ? 2 : (cur_hf_idx_ < 6 ? 3 : 4));
                remove_bits(hf_bits);

                cur_pos_ = hf_pos_[cur_hf_idx_ + 1];
                if (cur_hf_idx_ >= 2) {
                    state_ = state::HUFF_READ_EXTRA;
                } else {
                    // Output byte directly
                    unsigned byte_val = (ch_set_[cur_pos_] >> 8) & 0xFF;

                    // Update character set frequency
                    if (++ntopl_[place_[byte_val]] >= 0x80) {
                        corr_huff(ch_set_, ntopl_);
                    } else {
                        if (place_[byte_val] < 255) {
                            place_[byte_val]++;
                        }
                    }

                    // Check for stream mode termination
                    if (stmode_) {
                        if (byte_val == 0 && num_huf_++ >= 16) {
                            u32 check_bits = 0;
                            if (try_peek_bits(in_ptr, in_end, 16, check_bits)) {
                                if (check_bits > 0xFFF0) {
                                    state_ = state::DONE;
                                    return stream_result::done(bytes_read(), bytes_written());
                                }
                            }
                        }
                    }

                    // Output byte
                    if (out_ptr >= out_end) {
                        return stream_result::need_output(bytes_read(), bytes_written());
                    }
                    *out_ptr++ = static_cast<u8>(byte_val);
                    window_[unp_ptr_++ & WINDOW_MASK] = static_cast<u8>(byte_val);

                    state_ = state::READ_FLAG_BYTE;
                    if (out_ptr >= out_end) {
                        return stream_result::need_output(bytes_read(), bytes_written());
                    }
                }
                break;
            }

            case state::HUFF_READ_EXTRA: {
                unsigned extra_bits = cur_hf_idx_ / 2;
                u32 extra = 0;
                if (!try_read_bits(in_ptr, in_end, extra_bits, extra)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                cur_pos_ += extra;

                // Get byte from character set
                unsigned byte_val = (ch_set_[cur_pos_] >> 8) & 0xFF;

                // Update character set frequency
                if (++ntopl_[place_[byte_val]] >= 0x80) {
                    corr_huff(ch_set_, ntopl_);
                } else {
                    if (place_[byte_val] < 255) {
                        place_[byte_val]++;
                    }
                }

                // Check for stream mode termination
                if (stmode_) {
                    if (byte_val == 0 && num_huf_++ >= 16) {
                        u32 check_bits = 0;
                        if (try_peek_bits(in_ptr, in_end, 16, check_bits)) {
                            if (check_bits > 0xFFF0) {
                                state_ = state::DONE;
                                return stream_result::done(bytes_read(), bytes_written());
                            }
                        }
                    }
                }

                // Output byte
                if (out_ptr >= out_end) {
                    return stream_result::need_output(bytes_read(), bytes_written());
                }
                *out_ptr++ = static_cast<u8>(byte_val);
                window_[unp_ptr_++ & WINDOW_MASK] = static_cast<u8>(byte_val);

                state_ = state::READ_FLAG_BYTE;
                if (out_ptr >= out_end) {
                    return stream_result::need_output(bytes_read(), bytes_written());
                }
                break;
            }

            case state::SHORT_LZ_CHECK_REPEAT: {
                if (lcount_ == 2) {
                    // Repeat last match
                    lcount_ = 0;
                    cur_length_ = last_length_;
                    cur_distance_ = last_dist_;
                    match_remaining_ = cur_length_;
                    state_ = state::COPY_MATCH;
                } else {
                    state_ = state::SHORT_LZ_READ_LENGTH;
                }
                break;
            }

            case state::SHORT_LZ_READ_LENGTH: {
                u32 bits = 0;
                if (!try_peek_bits(in_ptr, in_end, 16, bits)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                if (bits < 0x8000) {
                    lcount_ = 0;
                    if (avr_ln1_ < 37) {
                        cur_length_ = (bits >> 12) + 2;
                        remove_bits(4);
                    } else {
                        cur_length_ = (bits >> 11) + 2;
                        remove_bits(5);
                    }
                    avr_ln1_ = (avr_ln1_ * 3 + cur_length_) / 4;
                    state_ = state::SHORT_LZ_READ_DIST;
                } else {
                    remove_bits(1);
                    state_ = state::SHORT_LZ_READ_LENGTH2;
                }
                break;
            }

            case state::SHORT_LZ_READ_LENGTH2: {
                u32 bits = 0;
                if (!try_peek_bits(in_ptr, in_end, 16, bits)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                if (bits < 0x4000) {
                    cur_length_ = 2;
                } else if (bits < 0x8000) {
                    cur_length_ = 3;
                } else if (bits < 0xC000) {
                    cur_length_ = 4;
                } else {
                    cur_length_ = 5;
                }
                remove_bits(2);
                lcount_ = cur_length_ == 5 ? 2 : 0;
                avr_ln1_ = (avr_ln1_ * 3 + cur_length_) / 4;
                state_ = state::SHORT_LZ_READ_DIST;
                break;
            }

            case state::SHORT_LZ_READ_DIST: {
                u32 bits = 0;
                if (!try_peek_bits(in_ptr, in_end, 16, bits)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                cur_slot_ = 0;
                for (unsigned i = 0; i < 8; i++) {
                    if (bits < short_dec_[i]) {
                        break;
                    }
                    cur_slot_ = i + 1;
                }

                unsigned dist_bits = cur_slot_ < 2 ? 1 : (cur_slot_ < 4 ? 2 : (cur_slot_ < 6 ? 3 : 4));
                remove_bits(dist_bits);

                if (cur_slot_ < 2) {
                    cur_distance_ = cur_slot_ + 1;
                    insert_old_dist(cur_distance_);
                    last_dist_ = cur_distance_;
                    last_length_ = cur_length_;
                    match_remaining_ = cur_length_;
                    state_ = state::COPY_MATCH;
                } else {
                    state_ = state::SHORT_LZ_READ_EXTRA;
                }
                break;
            }

            case state::SHORT_LZ_READ_EXTRA: {
                unsigned extra_bits = cur_slot_ / 2;
                u32 extra = 0;
                if (!try_read_bits(in_ptr, in_end, extra_bits, extra)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                cur_distance_ = short_pos_[cur_slot_ + 1] + extra + 1;

                insert_old_dist(cur_distance_);
                last_dist_ = cur_distance_;
                last_length_ = cur_length_;
                match_remaining_ = cur_length_;
                state_ = state::COPY_MATCH;
                break;
            }

            case state::LONG_LZ_READ_LENGTH: {
                u32 bits = 0;
                if (!try_peek_bits(in_ptr, in_end, 16, bits)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                if (avr_ln2_ < 122) {
                    if (bits < 0x8000) {
                        cur_length_ = (bits >> 12) + 3;
                        remove_bits(4);
                    } else {
                        cur_length_ = 3;
                        remove_bits(1);
                    }
                } else if (avr_ln2_ < 64) {
                    if (bits < 0x4000) {
                        cur_length_ = (bits >> 10) + 3;
                        remove_bits(6);
                    } else {
                        cur_length_ = 3;
                        remove_bits(2);
                    }
                } else {
                    if (bits < 0x1000) {
                        cur_length_ = (bits >> 8) + 3;
                        remove_bits(8);
                    } else {
                        cur_length_ = (bits >> 12) + 3;
                        remove_bits(4);
                    }
                }

                state_ = state::LONG_LZ_READ_PLACE;
                break;
            }

            case state::LONG_LZ_READ_PLACE: {
                u32 bits = 0;
                if (!try_peek_bits(in_ptr, in_end, 16, bits)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                cur_slot_ = 0;
                for (unsigned i = 0; i < 8; i++) {
                    if (bits < long_dec_[i]) {
                        break;
                    }
                    cur_slot_ = i + 1;
                }

                unsigned place_bits = cur_slot_ < 2 ? 1 : (cur_slot_ < 4 ? 2 : (cur_slot_ < 6 ? 3 : 4));
                remove_bits(place_bits);

                cur_pos_ = long_pos_[cur_slot_ + 1];
                if (cur_slot_ >= 2) {
                    state_ = state::LONG_LZ_READ_PLACE_EXTRA;
                } else {
                    // cur_pos_ is dist_idx
                    if (cur_pos_ < 4) {
                        cur_distance_ = old_dist_[cur_pos_];
                        // Reorder old distances
                        for (unsigned i = cur_pos_; i > 0; i--) {
                            old_dist_[i] = old_dist_[i - 1];
                        }
                        old_dist_[0] = cur_distance_;

                        // Adjust length based on distance
                        if (cur_distance_ >= max_dist3_) {
                            cur_length_++;
                        }
                        if (cur_distance_ <= 256) {
                            cur_length_ += 8;
                        }

                        avr_ln2_ = (avr_ln2_ * 3 + cur_length_) / 4;
                        last_dist_ = cur_distance_;
                        last_length_ = cur_length_;
                        match_remaining_ = cur_length_;
                        state_ = state::COPY_MATCH;
                    } else {
                        state_ = state::LONG_LZ_READ_LOW;
                    }
                }
                break;
            }

            case state::LONG_LZ_READ_PLACE_EXTRA: {
                unsigned extra_bits = cur_slot_ / 2;
                u32 extra = 0;
                if (!try_read_bits(in_ptr, in_end, extra_bits, extra)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                cur_pos_ += extra;

                if (cur_pos_ < 4) {
                    cur_distance_ = old_dist_[cur_pos_];
                    // Reorder old distances
                    for (unsigned i = cur_pos_; i > 0; i--) {
                        old_dist_[i] = old_dist_[i - 1];
                    }
                    old_dist_[0] = cur_distance_;

                    // Adjust length based on distance
                    if (cur_distance_ >= max_dist3_) {
                        cur_length_++;
                    }
                    if (cur_distance_ <= 256) {
                        cur_length_ += 8;
                    }

                    avr_ln2_ = (avr_ln2_ * 3 + cur_length_) / 4;
                    last_dist_ = cur_distance_;
                    last_length_ = cur_length_;
                    match_remaining_ = cur_length_;
                    state_ = state::COPY_MATCH;
                } else {
                    state_ = state::LONG_LZ_READ_LOW;
                }
                break;
            }

            case state::LONG_LZ_READ_LOW: {
                // Read new distance (high bits from ChSetB, low 8 bits from stream)
                u32 low = 0;
                if (!try_read_bits(in_ptr, in_end, 8, low)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                unsigned high = (ch_set_b_[cur_pos_ - 4] >> 8) & 0xFF;
                cur_distance_ = (high << 8) + low + 1;
                insert_old_dist(cur_distance_);

                // Adjust length based on distance
                if (cur_distance_ >= max_dist3_) {
                    cur_length_++;
                }
                if (cur_distance_ <= 256) {
                    cur_length_ += 8;
                }

                avr_ln2_ = (avr_ln2_ * 3 + cur_length_) / 4;
                last_dist_ = cur_distance_;
                last_length_ = cur_length_;
                match_remaining_ = cur_length_;
                state_ = state::COPY_MATCH;
                break;
            }

            case state::COPY_MATCH: {
                while (match_remaining_ > 0) {
                    if (out_ptr >= out_end) {
                        return stream_result::need_output(bytes_read(), bytes_written());
                    }

                    size_t src_ptr = unp_ptr_ - cur_distance_;
                    u8 value = window_[src_ptr & WINDOW_MASK];
                    *out_ptr++ = value;
                    window_[unp_ptr_++ & WINDOW_MASK] = value;
                    match_remaining_--;
                }

                state_ = state::READ_FLAG_BYTE;
                if (out_ptr >= out_end) {
                    return stream_result::need_output(bytes_read(), bytes_written());
                }
                break;
            }

            case state::DONE:
                break;
        }
    }

    return stream_result::done(bytes_read(), bytes_written());
}

}  // namespace crate
