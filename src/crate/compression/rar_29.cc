#include <crate/compression/rar_29.hh>

namespace crate {

void rar_29_decompressor::init_state() {
    // Initialize window
    size_t min_size = 0x400000; // 4MB minimum for RAR3
    if (window_.size() < min_size) {
        window_.resize(min_size);
    }

    if (!solid_mode_) {
        std::fill(window_.begin(), window_.end(), u8(0));
        unp_ptr_ = 0;
        wr_ptr_ = 0;
        first_win_done_ = false;
        tables_read_ = false;
        ppm_mode_ = false;
        ppm_esc_char_ = 2;
        old_dist_.fill(size_t(-1));
        old_dist_ptr_ = 0;
        last_length_ = 0;
        low_dist_rep_count_ = 0;
        prev_low_dist_ = 0;
        old_table_.fill(0);
    }

    bit_buffer_ = 0;
    bits_left_ = 0;
    total_bits_consumed_ = 0;

    state_ = state::READ_TABLES_ALIGN;

    bit_lengths_.fill(0);
    main_table_.fill(0);
    table_index_ = 0;
    cur_length_ = 0;
    repeat_count_ = 0;

    cur_symbol_ = 0;
    cur_length_slot_ = 0;
    cur_length_bits_ = 0;
    cur_dist_slot_ = 0;
    cur_dist_bits_ = 0;
    cur_distance_ = 0;
    match_remaining_ = 0;
    dist_num_ = 0;

    initialized_ = true;
}

void rar_29_decompressor::init_tables() {
    // Initialize length decode tables
    ldecode_ = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28,
        32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224
    };
    lbits_ = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5
    };

    // Initialize distance decode tables
    static const int dbit_length_counts[] = {4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 14, 0, 12};
    int dist = 0, bit_length = 0, slot = 0;
    const int max_slot = static_cast<int>(rar::DC30);
    for (int i = 0; i < 19 && slot < max_slot; i++, bit_length++) {
        for (int j = 0; j < dbit_length_counts[i] && slot < max_slot;
             j++, slot++, dist += (1 << bit_length)) {
            auto slot_index = static_cast<size_t>(slot);
            ddecode_[slot_index] = dist;
            dbits_[slot_index] = static_cast<u8>(bit_length);
        }
    }
}

bool rar_29_decompressor::try_peek_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
    if (n == 0) {
        out = 0;
        return true;
    }

    // Fill bit buffer with available bytes
    while (bits_left_ < n && ptr < end) {
        bit_buffer_ = (bit_buffer_ << 8) | *ptr++;
        bits_left_ += 8;
    }

    // If we still don't have enough bits, pad with zeros for the missing bits
    if (bits_left_ < n) {
        if (bits_left_ > 0) {
            out = static_cast<u32>((bit_buffer_ << (n - bits_left_)) & ((1ULL << n) - 1));
        } else {
            out = 0;
        }
        return true;
    }

    out = static_cast<u32>((bit_buffer_ >> (bits_left_ - n)) & ((1ULL << n) - 1));
    return true;
}

bool rar_29_decompressor::try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
    if (!try_peek_bits(ptr, end, n, out)) {
        return false;
    }
    remove_bits(n);
    return true;
}

void rar_29_decompressor::remove_bits(unsigned n) {
    if (n <= bits_left_) {
        bits_left_ -= n;
    } else {
        bits_left_ = 0;
    }
    total_bits_consumed_ += n;
}

bool rar_29_decompressor::try_decode_number(const byte*& ptr, const byte* end,
                                            const rar_decode_table& dec, unsigned& out) {
    u32 bits = 0;
    if (!try_peek_bits(ptr, end, 16, bits)) {
        return false;
    }

    unsigned bit_field = bits & 0xFFFE;

    // Quick decode path
    if (bit_field < dec.decode_len[dec.quick_bits]) {
        unsigned code = bit_field >> (16 - dec.quick_bits);
        remove_bits(dec.quick_len[code]);
        out = dec.quick_num[code];
        return true;
    }

    // Slow path
    unsigned num_bits = 15;
    for (unsigned i = dec.quick_bits + 1; i < 15; i++) {
        if (bit_field < dec.decode_len[i]) {
            num_bits = i;
            break;
        }
    }

    remove_bits(num_bits);

    unsigned dist = bit_field - dec.decode_len[num_bits - 1];
    dist >>= (16 - num_bits);

    unsigned pos = dec.decode_pos[num_bits] + dist;
    if (pos >= dec.max_num) {
        pos = 0;
    }

    out = dec.decode_num[pos];
    return true;
}

void rar_29_decompressor::insert_old_dist(size_t distance) {
    old_dist_[3] = old_dist_[2];
    old_dist_[2] = old_dist_[1];
    old_dist_[1] = old_dist_[0];
    old_dist_[0] = distance;
}

result_t<stream_result> rar_29_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    if (!initialized_) {
        init_state();
    } else if (state_ == state::DONE) {
        // Starting a new file - reset input-related state
        bit_buffer_ = 0;
        bits_left_ = 0;
        total_bits_consumed_ = 0;
        state_ = state::READ_TABLES_ALIGN;
        // Note: tables_read_, old_table_, and window preserved for solid mode
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

    size_t win_mask = window_.size() - 1;

    while (state_ != state::DONE) {
        switch (state_) {
            case state::READ_TABLES_ALIGN: {
                // Align to byte boundary
                unsigned bit_offset = bits_left_ % 8;
                if (bit_offset != 0) {
                    remove_bits(bit_offset);
                }
                state_ = state::READ_TABLES_CHECK_PPM;
                break;
            }

            case state::READ_TABLES_CHECK_PPM: {
                u32 bit = 0;
                if (!try_peek_bits(in_ptr, in_end, 1, bit)) {
                    if (input_finished) {
                        if (bytes_written() > 0) {
                            state_ = state::DONE;
                            return stream_result::done(bytes_read(), bytes_written());
                        }
                        return stream_result::done(bytes_read(), bytes_written());
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                if (bit & 1) {
                    // PPM mode - requires all input for now
                    remove_bits(1);
                    state_ = state::PPM_MODE;
                } else {
                    state_ = state::READ_TABLES_CHECK_OLD;
                }
                break;
            }

            case state::PPM_MODE: {
                // PPM mode requires all input - fall back to non-streaming
                if (!input_finished) {
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                // Store input for PPM adapter
                input_span_ = input;

                // Create bit-stream input adapter
                // PPM data starts at current bit position
                // Use total_bits_consumed_ to calculate exact position
                size_t byte_pos = total_bits_consumed_ / 8;
                unsigned bit_pos = static_cast<unsigned>(total_bits_consumed_ % 8);
                ppm_input_ = std::make_unique<rar::ppm::bit_stream_input>(
                    input_span_, byte_pos, bit_pos);

                int esc_char = ppm_esc_char_;
                if (!ppm_model_.decode_init(ppm_input_.get(), esc_char)) {
                    return std::unexpected(error{error_code::CorruptData, "Failed to initialize PPM decoder"});
                }
                ppm_esc_char_ = esc_char;
                ppm_mode_ = true;

                // Decompress PPM data
                while (out_ptr < out_end) {
                    int ch = ppm_model_.decode_char();
                    if (ch < 0) {
                        ppm_model_.cleanup();
                        break;
                    }

                    if (ch == ppm_esc_char_) {
                        ch = ppm_model_.decode_char();
                        if (ch < 0) break;

                        if (ch == 0) {
                            // Switch to LZ mode
                            ppm_mode_ = false;
                            state_ = state::READ_TABLES_ALIGN;
                            break;
                        }
                        if (ch == 2) {
                            // End of PPM data
                            state_ = state::DONE;
                            break;
                        }
                        if (ch == 3) {
                            // VM filter - skip
                            continue;
                        }
                        if (ch == 4) {
                            // Read new distance
                            unsigned dist = 0;
                            for (int i = 0; i < 4; i++) {
                                ch = ppm_model_.decode_char();
                                if (ch < 0) break;
                                dist |= static_cast<unsigned>(ch) << (i * 8);
                            }
                            if (ch < 0) break;
                            insert_old_dist(dist + 1);
                            continue;
                        }
                        if (ch == 5) {
                            ch = ppm_model_.decode_char();
                            if (ch < 0) break;
                            insert_old_dist(static_cast<unsigned>(ch) + 1);
                            continue;
                        }
                        if (ch >= 6) {
                            unsigned length = static_cast<unsigned>(ch) - 6 + 2;
                            if (length >= 3) {
                                ch = ppm_model_.decode_char();
                                if (ch < 0) break;
                                length += static_cast<unsigned>(ch) << 2;
                            }
                            if (old_dist_[0] != size_t(-1)) {
                                size_t distance = old_dist_[0];
                                while (length-- > 0 && out_ptr < out_end) {
                                    size_t src = unp_ptr_ - distance;
                                    u8 value = window_[src & win_mask];
                                    *out_ptr++ = value;
                                    window_[unp_ptr_++ & win_mask] = value;
                                }
                                last_length_ = length;
                            }
                            continue;
                        }
                    } else {
                        *out_ptr++ = static_cast<u8>(ch);
                        window_[unp_ptr_++ & win_mask] = static_cast<u8>(ch);
                    }
                }

                if (state_ == state::PPM_MODE) {
                    state_ = state::DONE;
                }
                break;
            }

            case state::READ_TABLES_CHECK_OLD: {
                u32 bits = 0;
                if (!try_read_bits(in_ptr, in_end, 2, bits)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                // Bit 0 was PPM flag (already consumed), bit 1 is preserve old table flag
                if (!(bits & 1)) {
                    old_table_.fill(0);
                }

                ppm_mode_ = false;
                table_index_ = 0;
                state_ = state::READ_TABLES_BIT_LENGTH;
                break;
            }

            case state::READ_TABLES_BIT_LENGTH: {
                while (table_index_ < rar::BC30) {
                    u32 len = 0;
                    if (!try_read_bits(in_ptr, in_end, 4, len)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }

                    if (len == 15) {
                        cur_length_ = 15;
                        state_ = state::READ_TABLES_BIT_LENGTH_ZERO;
                        break;
                    } else {
                        bit_lengths_[table_index_++] = static_cast<u8>(len);
                    }
                }

                if (table_index_ >= rar::BC30 && state_ == state::READ_TABLES_BIT_LENGTH) {
                    make_decode_tables(bit_lengths_.data(), tables_.bd, rar::BC30);
                    table_index_ = 0;
                    state_ = state::READ_TABLES_MAIN_SYMBOL;
                }
                break;
            }

            case state::READ_TABLES_BIT_LENGTH_ZERO: {
                u32 zero_count = 0;
                if (!try_read_bits(in_ptr, in_end, 4, zero_count)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                if (zero_count == 0) {
                    bit_lengths_[table_index_++] = 15;
                } else {
                    zero_count += 2;
                    while (zero_count-- > 0 && table_index_ < rar::BC30) {
                        bit_lengths_[table_index_++] = 0;
                    }
                }

                state_ = state::READ_TABLES_BIT_LENGTH;
                break;
            }

            case state::READ_TABLES_MAIN_SYMBOL: {
                while (table_index_ < rar::HUFF_TABLE_SIZE30) {
                    unsigned num = 0;
                    if (!try_decode_number(in_ptr, in_end, tables_.bd, num)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }

                    if (num < 16) {
                        // Delta from old table
                        main_table_[table_index_] = static_cast<u8>((num + old_table_[table_index_]) & 0xF);
                        table_index_++;
                    } else if (num < 18) {
                        cur_symbol_ = num;
                        state_ = state::READ_TABLES_MAIN_REPEAT;
                        break;
                    } else {
                        cur_symbol_ = num;
                        state_ = state::READ_TABLES_MAIN_ZEROS;
                        break;
                    }
                }

                if (table_index_ >= rar::HUFF_TABLE_SIZE30 && state_ == state::READ_TABLES_MAIN_SYMBOL) {
                    state_ = state::BUILD_TABLES;
                }
                break;
            }

            case state::READ_TABLES_MAIN_REPEAT: {
                unsigned bits_needed = (cur_symbol_ == 16) ? 3 : 7;
                u32 count = 0;
                if (!try_read_bits(in_ptr, in_end, bits_needed, count)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                if (cur_symbol_ == 16) {
                    count += 3;
                } else {
                    count += 11;
                }

                if (table_index_ == 0) {
                    return std::unexpected(error{error_code::InvalidHuffmanTable, "Cannot repeat at start"});
                }

                while (count-- > 0 && table_index_ < rar::HUFF_TABLE_SIZE30) {
                    main_table_[table_index_] = main_table_[table_index_ - 1];
                    table_index_++;
                }

                state_ = state::READ_TABLES_MAIN_SYMBOL;
                break;
            }

            case state::READ_TABLES_MAIN_ZEROS: {
                unsigned bits_needed = (cur_symbol_ == 18) ? 3 : 7;
                u32 count = 0;
                if (!try_read_bits(in_ptr, in_end, bits_needed, count)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                if (cur_symbol_ == 18) {
                    count += 3;
                } else {
                    count += 11;
                }

                while (count-- > 0 && table_index_ < rar::HUFF_TABLE_SIZE30) {
                    main_table_[table_index_++] = 0;
                }

                state_ = state::READ_TABLES_MAIN_SYMBOL;
                break;
            }

            case state::BUILD_TABLES: {
                // Save for next block (delta encoding)
                old_table_ = main_table_;

                make_decode_tables(main_table_.data(), tables_.ld, rar::NC30);
                make_decode_tables(main_table_.data() + rar::NC30, tables_.dd, rar::DC30);
                make_decode_tables(main_table_.data() + rar::NC30 + rar::DC30, tables_.ldd, rar::LDC30);
                make_decode_tables(main_table_.data() + rar::NC30 + rar::DC30 + rar::LDC30, tables_.rd, rar::RC30);

                tables_read_ = true;
                state_ = state::DECODE_SYMBOL;
                break;
            }

            case state::DECODE_SYMBOL: {
                unsigned number = 0;
                if (!try_decode_number(in_ptr, in_end, tables_.ld, number)) {
                    if (input_finished) {
                        if (bytes_written() > 0) {
                            state_ = state::DONE;
                            return stream_result::done(bytes_read(), bytes_written());
                        }
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                if (number < 256) {
                    cur_symbol_ = number;
                    state_ = state::OUTPUT_LITERAL;
                } else if (number >= 271) {
                    // Match
                    cur_length_slot_ = number - 271;
                    cur_length_ = ldecode_[cur_length_slot_] + 3;
                    cur_length_bits_ = lbits_[cur_length_slot_];
                    if (cur_length_bits_ > 0) {
                        state_ = state::MATCH_READ_LENGTH_EXTRA;
                    } else {
                        state_ = state::MATCH_DECODE_DIST;
                    }
                } else if (number == 256) {
                    // End of block
                    state_ = state::END_OF_BLOCK_CHECK;
                } else if (number == 257) {
                    // VM filter - skip
                    // In full implementation, would read filter data
                    state_ = state::DECODE_SYMBOL;
                } else if (number == 258) {
                    // Repeat last
                    state_ = state::REPEAT_LAST;
                } else if (number < 263) {
                    // Repeat with old distance
                    dist_num_ = number - 259;
                    if (dist_num_ < old_dist_.size() && old_dist_[dist_num_] != size_t(-1)) {
                        cur_distance_ = old_dist_[dist_num_];
                        // Reorder distances
                        for (unsigned i = dist_num_; i > 0; i--) {
                            old_dist_[i] = old_dist_[i - 1];
                        }
                        old_dist_[0] = cur_distance_;
                        state_ = state::REPEAT_OLD_DIST_DECODE_LENGTH;
                    } else {
                        state_ = state::DECODE_SYMBOL;
                    }
                } else {
                    // Short distance (263-270)
                    unsigned sd_num = number - 263;
                    cur_distance_ = sd_decode_[sd_num] + 1;
                    cur_dist_bits_ = sd_bits_[sd_num];
                    if (cur_dist_bits_ > 0) {
                        state_ = state::SHORT_DIST_READ_EXTRA;
                    } else {
                        insert_old_dist(cur_distance_);
                        last_length_ = 2;
                        cur_length_ = 2;
                        match_remaining_ = 2;
                        state_ = state::COPY_MATCH;
                    }
                }
                break;
            }

            case state::OUTPUT_LITERAL: {
                if (out_ptr >= out_end) {
                    return stream_result::need_output(bytes_read(), bytes_written());
                }

                *out_ptr++ = static_cast<u8>(cur_symbol_);
                window_[unp_ptr_++ & win_mask] = static_cast<u8>(cur_symbol_);

                state_ = state::DECODE_SYMBOL;
                break;
            }

            case state::MATCH_READ_LENGTH_EXTRA: {
                u32 extra = 0;
                if (!try_read_bits(in_ptr, in_end, cur_length_bits_, extra)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                cur_length_ += extra;
                state_ = state::MATCH_DECODE_DIST;
                break;
            }

            case state::MATCH_DECODE_DIST: {
                unsigned dist_number = 0;
                if (!try_decode_number(in_ptr, in_end, tables_.dd, dist_number)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                cur_dist_slot_ = dist_number;
                cur_distance_ = static_cast<size_t>(ddecode_[dist_number] + 1);
                cur_dist_bits_ = dbits_[dist_number];

                if (cur_dist_bits_ > 0) {
                    if (dist_number > 9) {
                        if (cur_dist_bits_ > 4) {
                            state_ = state::MATCH_READ_DIST_EXTRA_HIGH;
                        } else {
                            state_ = state::MATCH_READ_LOW_DIST;
                        }
                    } else {
                        state_ = state::MATCH_READ_DIST_EXTRA;
                    }
                } else {
                    // Length adjustment for long distances
                    if (cur_distance_ >= 0x2000) {
                        cur_length_++;
                        if (cur_distance_ >= 0x40000) {
                            cur_length_++;
                        }
                    }

                    insert_old_dist(cur_distance_);
                    last_length_ = cur_length_;
                    match_remaining_ = cur_length_;
                    state_ = state::COPY_MATCH;
                }
                break;
            }

            case state::MATCH_READ_DIST_EXTRA: {
                u32 extra = 0;
                if (!try_read_bits(in_ptr, in_end, cur_dist_bits_, extra)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                cur_distance_ += extra;

                // Length adjustment for long distances
                if (cur_distance_ >= 0x2000) {
                    cur_length_++;
                    if (cur_distance_ >= 0x40000) {
                        cur_length_++;
                    }
                }

                insert_old_dist(cur_distance_);
                last_length_ = cur_length_;
                match_remaining_ = cur_length_;
                state_ = state::COPY_MATCH;
                break;
            }

            case state::MATCH_READ_DIST_EXTRA_HIGH: {
                unsigned high_bits = cur_dist_bits_ - 4;
                u32 extra = 0;
                if (!try_read_bits(in_ptr, in_end, high_bits, extra)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                cur_distance_ += extra << 4;
                state_ = state::MATCH_READ_LOW_DIST;
                break;
            }

            case state::MATCH_READ_LOW_DIST: {
                if (low_dist_rep_count_ > 0) {
                    low_dist_rep_count_--;
                    cur_distance_ += prev_low_dist_;

                    // Length adjustment
                    if (cur_distance_ >= 0x2000) {
                        cur_length_++;
                        if (cur_distance_ >= 0x40000) {
                            cur_length_++;
                        }
                    }

                    insert_old_dist(cur_distance_);
                    last_length_ = cur_length_;
                    match_remaining_ = cur_length_;
                    state_ = state::COPY_MATCH;
                } else {
                    unsigned low_dist = 0;
                    if (!try_decode_number(in_ptr, in_end, tables_.ldd, low_dist)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }

                    if (low_dist == 16) {
                        low_dist_rep_count_ = rar::LOW_DIST_REP_COUNT - 1;
                        cur_distance_ += prev_low_dist_;
                    } else {
                        cur_distance_ += low_dist;
                        prev_low_dist_ = low_dist;
                    }

                    // Length adjustment
                    if (cur_distance_ >= 0x2000) {
                        cur_length_++;
                        if (cur_distance_ >= 0x40000) {
                            cur_length_++;
                        }
                    }

                    insert_old_dist(cur_distance_);
                    last_length_ = cur_length_;
                    match_remaining_ = cur_length_;
                    state_ = state::COPY_MATCH;
                }
                break;
            }

            case state::END_OF_BLOCK_CHECK: {
                u32 bit = 0;
                if (!try_peek_bits(in_ptr, in_end, 1, bit)) {
                    if (input_finished) {
                        state_ = state::DONE;
                        return stream_result::done(bytes_read(), bytes_written());
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                if (bit & 1) {
                    // Bit 1: new table here, continue with same file
                    remove_bits(1);
                    state_ = state::READ_TABLES_ALIGN;
                } else {
                    // Bits 00 or 01: new file (end of current file)
                    remove_bits(2);
                    state_ = state::DONE;
                }
                break;
            }

            case state::REPEAT_LAST: {
                if (last_length_ != 0 && old_dist_[0] != size_t(-1)) {
                    cur_length_ = last_length_;
                    cur_distance_ = old_dist_[0];
                    match_remaining_ = cur_length_;
                    state_ = state::COPY_MATCH;
                } else {
                    state_ = state::DECODE_SYMBOL;
                }
                break;
            }

            case state::REPEAT_OLD_DIST_DECODE_LENGTH: {
                unsigned length_number = 0;
                if (!try_decode_number(in_ptr, in_end, tables_.rd, length_number)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                cur_length_slot_ = length_number;
                cur_length_ = ldecode_[length_number] + 2;
                cur_length_bits_ = lbits_[length_number];

                if (cur_length_bits_ > 0) {
                    state_ = state::REPEAT_OLD_DIST_LENGTH_EXTRA;
                } else {
                    last_length_ = cur_length_;
                    match_remaining_ = cur_length_;
                    state_ = state::COPY_MATCH;
                }
                break;
            }

            case state::REPEAT_OLD_DIST_LENGTH_EXTRA: {
                u32 extra = 0;
                if (!try_read_bits(in_ptr, in_end, cur_length_bits_, extra)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                cur_length_ += extra;
                last_length_ = cur_length_;
                match_remaining_ = cur_length_;
                state_ = state::COPY_MATCH;
                break;
            }

            case state::SHORT_DIST_READ_EXTRA: {
                u32 extra = 0;
                if (!try_read_bits(in_ptr, in_end, cur_dist_bits_, extra)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                cur_distance_ += extra;

                insert_old_dist(cur_distance_);
                last_length_ = 2;
                cur_length_ = 2;
                match_remaining_ = 2;
                state_ = state::COPY_MATCH;
                break;
            }

            case state::COPY_MATCH: {
                while (match_remaining_ > 0) {
                    if (out_ptr >= out_end) {
                        return stream_result::need_output(bytes_read(), bytes_written());
                    }

                    size_t src_ptr = unp_ptr_ - cur_distance_;
                    u8 value = window_[src_ptr & win_mask];
                    *out_ptr++ = value;
                    window_[unp_ptr_++ & win_mask] = value;
                    match_remaining_--;
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
