#include <crate/compression/rar5.hh>

namespace crate {

void rar5_decompressor::init_state() {
    // Initialize window
    size_t min_size = 0x100000; // 1MB minimum
    if (window_.size() < min_size) {
        window_.resize(min_size);
    }
    max_win_size_ = window_.size();
    max_win_mask_ = max_win_size_ - 1;

    if (!solid_mode_) {
        std::fill(window_.begin(), window_.end(), u8(0));
        unp_ptr_ = 0;
        wr_ptr_ = 0;
        first_win_done_ = false;
        old_dist_.fill(0);
        last_length_ = 0;
        tables_read_ = false;
        filter_processor_.clear();
        written_file_pos_ = 0;
    }

    bit_buffer_ = 0;
    bits_left_ = 0;
    total_bits_consumed_ = 0;

    state_ = state::READ_BLOCK_HEADER_ALIGN;

    block_header_ = BlockHeader{};
    block_start_bit_offset_ = 0;

    bit_lengths_.fill(0);
    main_table_.clear();
    table_index_ = 0;
    table_size_ = extra_dist_ ? rar::HUFF_TABLE_SIZEX : rar::HUFF_TABLE_SIZEB;
    main_table_.resize(table_size_, 0);
    cur_length_ = 0;
    repeat_count_ = 0;

    header_flags_ = 0;
    header_checksum_ = 0;
    header_byte_count_ = 0;
    header_bytes_read_ = 0;

    cur_symbol_ = 0;
    cur_dist_slot_ = 0;
    cur_d_bits_ = 0;
    cur_distance_ = 0;
    match_remaining_ = 0;
    dist_num_ = 0;

    filter_block_start_ = 0;
    filter_block_length_ = 0;
    filter_type_ = 0;
    vint_byte_count_ = 0;
    vint_bytes_read_ = 0;
    vint_value_ = 0;

    length_slot_ = 0;
    length_lbits_ = 0;

    initialized_ = true;
}

bool rar5_decompressor::try_peek_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
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
    // This matches the original RAR behavior at block boundaries
    if (bits_left_ < n) {
        // Extract what we have, shifted to MSB position, with zeros for missing bits
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

bool rar5_decompressor::try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
    if (!try_peek_bits(ptr, end, n, out)) {
        return false;
    }
    remove_bits(n);
    return true;
}

void rar5_decompressor::remove_bits(unsigned n) {
    if (n <= bits_left_) {
        bits_left_ -= n;
    } else {
        bits_left_ = 0;  // Consuming more than we have (at end of stream)
    }
    total_bits_consumed_ += n;
}

void rar5_decompressor::insert_old_dist(size_t distance) {
    for (size_t i = old_dist_.size() - 1; i > 0; i--) {
        old_dist_[i] = old_dist_[i - 1];
    }
    old_dist_[0] = distance;
}

bool rar5_decompressor::try_decode_number(const byte*& ptr, const byte* end,
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

result_t<stream_result> rar5_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    if (!initialized_) {
        init_state();
    } else if (state_ == state::DONE) {
        // Starting a new file (in solid mode or fresh decompression)
        // Reset input-related state but preserve window and tables for solid mode
        bit_buffer_ = 0;
        bits_left_ = 0;
        total_bits_consumed_ = 0;
        state_ = state::READ_BLOCK_HEADER_ALIGN;
        block_header_ = BlockHeader{};
        block_start_bit_offset_ = 0;
        // Note: tables_read_ is preserved for solid mode (tables carry over)
        // For non-solid mode, the first block will have table_present=true
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
            case state::READ_BLOCK_HEADER_ALIGN: {
                // Discard bits to align to byte boundary
                unsigned bit_offset = bits_left_ % 8;
                if (bit_offset != 0) {
                    remove_bits(bit_offset);
                }
                state_ = state::READ_BLOCK_HEADER_FLAGS;
                break;
            }

            case state::READ_BLOCK_HEADER_FLAGS: {
                u32 flags = 0;
                if (!try_read_bits(in_ptr, in_end, 8, flags)) {
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

                header_flags_ = static_cast<u8>(flags);
                block_header_.block_bit_size = (header_flags_ & 0x07) + 1;
                header_byte_count_ = ((header_flags_ >> 3) & 0x03) + 1;
                block_header_.last_block = (header_flags_ & 0x40) != 0;
                block_header_.table_present = (header_flags_ & 0x80) != 0;

                if (header_byte_count_ == 4) {
                    return crate::make_unexpected(error{error_code::CorruptData, "Invalid RAR5 block header byte count"});
                }

                state_ = state::READ_BLOCK_HEADER_CHECKSUM;
                break;
            }

            case state::READ_BLOCK_HEADER_CHECKSUM: {
                u32 checksum = 0;
                if (!try_read_bits(in_ptr, in_end, 8, checksum)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                header_checksum_ = static_cast<u8>(checksum);
                block_header_.block_size = 0;
                header_bytes_read_ = 0;
                state_ = state::READ_BLOCK_HEADER_SIZE;
                break;
            }

            case state::READ_BLOCK_HEADER_SIZE: {
                while (header_bytes_read_ < header_byte_count_) {
                    u32 byte_val = 0;
                    if (!try_read_bits(in_ptr, in_end, 8, byte_val)) {
                        if (input_finished) {
                            return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    block_header_.block_size += static_cast<int>(byte_val) << (header_bytes_read_ * 8);
                    header_bytes_read_++;
                }

                // Verify checksum
                u8 calc_checksum = 0x5A ^ header_flags_ ^
                                   static_cast<u8>(block_header_.block_size) ^
                                   static_cast<u8>(block_header_.block_size >> 8) ^
                                   static_cast<u8>(block_header_.block_size >> 16);
                if (calc_checksum != header_checksum_) {
                    return crate::make_unexpected(error{error_code::CorruptData, "RAR5 block header checksum mismatch"});
                }

                // Track block content start position using total bits consumed
                block_start_bit_offset_ = total_bits_consumed_;

                if (block_header_.table_present) {
                    table_index_ = 0;
                    state_ = state::READ_TABLES_BIT_LENGTH;
                } else if (!tables_read_) {
                    return crate::make_unexpected(error{error_code::InvalidHuffmanTable, "No Huffman tables in RAR5 stream"});
                } else {
                    state_ = state::DECODE_SYMBOL;
                }
                break;
            }

            case state::READ_TABLES_BIT_LENGTH: {
                while (table_index_ < rar::BC) {
                    u32 len = 0;
                    if (!try_read_bits(in_ptr, in_end, 4, len)) {
                        if (input_finished) {
                            return crate::make_unexpected(error{error_code::InputBufferUnderflow});
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

                if (table_index_ >= rar::BC && state_ == state::READ_TABLES_BIT_LENGTH) {
                    make_decode_tables(bit_lengths_.data(), tables_.bd, rar::BC);
                    table_index_ = 0;
                    std::fill(main_table_.begin(), main_table_.end(), u8(0));
                    state_ = state::READ_TABLES_MAIN_SYMBOL;
                }
                break;
            }

            case state::READ_TABLES_BIT_LENGTH_ZERO: {
                u32 zero_count = 0;
                if (!try_read_bits(in_ptr, in_end, 4, zero_count)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                if (zero_count == 0) {
                    bit_lengths_[table_index_++] = 15;
                } else {
                    zero_count += 2;
                    while (zero_count-- > 0 && table_index_ < rar::BC) {
                        bit_lengths_[table_index_++] = 0;
                    }
                }

                state_ = state::READ_TABLES_BIT_LENGTH;
                break;
            }

            case state::READ_TABLES_MAIN_SYMBOL: {
                while (table_index_ < table_size_) {
                    unsigned num = 0;
                    if (!try_decode_number(in_ptr, in_end, tables_.bd, num)) {
                        if (input_finished) {
                            return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }

                    if (num < 16) {
                        main_table_[table_index_++] = static_cast<u8>(num);
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

                if (table_index_ >= table_size_ && state_ == state::READ_TABLES_MAIN_SYMBOL) {
                    state_ = state::BUILD_TABLES;
                }
                break;
            }

            case state::READ_TABLES_MAIN_REPEAT: {
                // 16: repeat previous 3-10 times (3 bits)
                // 17: repeat previous 11-138 times (7 bits)
                unsigned bits_needed = (cur_symbol_ == 16) ? 3 : 7;
                u32 count = 0;
                if (!try_read_bits(in_ptr, in_end, bits_needed, count)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                if (cur_symbol_ == 16) {
                    count += 3;
                } else {
                    count += 11;
                }

                if (table_index_ == 0) {
                    return crate::make_unexpected(error{error_code::InvalidHuffmanTable, "Cannot repeat at start of table"});
                }

                while (count-- > 0 && table_index_ < table_size_) {
                    main_table_[table_index_] = main_table_[table_index_ - 1];
                    table_index_++;
                }

                state_ = state::READ_TABLES_MAIN_SYMBOL;
                break;
            }

            case state::READ_TABLES_MAIN_ZEROS: {
                // 18: zeros 3-10 times (3 bits)
                // 19: zeros 11-138 times (7 bits)
                unsigned bits_needed = (cur_symbol_ == 18) ? 3 : 7;
                u32 count = 0;
                if (!try_read_bits(in_ptr, in_end, bits_needed, count)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                if (cur_symbol_ == 18) {
                    count += 3;
                } else {
                    count += 11;
                }

                while (count-- > 0 && table_index_ < table_size_) {
                    main_table_[table_index_++] = 0;
                }

                state_ = state::READ_TABLES_MAIN_SYMBOL;
                break;
            }

            case state::BUILD_TABLES: {
                unsigned dc = extra_dist_ ? rar::DCX : rar::DCB;
                make_decode_tables(main_table_.data(), tables_.ld, rar::NC);
                make_decode_tables(main_table_.data() + rar::NC, tables_.dd, dc);
                make_decode_tables(main_table_.data() + rar::NC + dc, tables_.ldd, rar::LDC);
                make_decode_tables(main_table_.data() + rar::NC + dc + rar::LDC, tables_.rd, rar::RC);
                tables_read_ = true;
                state_ = state::DECODE_SYMBOL;
                break;
            }

            case state::DECODE_SYMBOL: {
                // Check block boundary first
                // bits_consumed is measured from block_start_bit_offset_
                size_t bits_consumed = total_bits_consumed_ - block_start_bit_offset_;

                size_t block_bits = static_cast<size_t>(block_header_.block_size - 1) * 8 +
                                    static_cast<size_t>(block_header_.block_bit_size);

                if (bits_consumed >= block_bits) {
                    state_ = state::CHECK_BLOCK_END;
                    break;
                }

                unsigned number = 0;
                if (!try_decode_number(in_ptr, in_end, tables_.ld, number)) {
                    if (input_finished) {
                        if (bytes_written() > 0) {
                            state_ = state::DONE;
                            return stream_result::done(bytes_read(), bytes_written());
                        }
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                if (number < 256) {
                    cur_symbol_ = number;
                    state_ = state::OUTPUT_LITERAL;
                } else if (number == 256) {
                    // Filter
                    vint_value_ = 0;
                    vint_bytes_read_ = 0;
                    state_ = state::FILTER_READ_START_COUNT;
                } else if (number == 257) {
                    // Repeat last
                    state_ = state::REPEAT_LAST;
                } else if (number < 262) {
                    // Short repeat
                    dist_num_ = number - 258;
                    cur_distance_ = old_dist_[dist_num_];
                    state_ = state::SHORT_REPEAT_DECODE_LENGTH;
                } else {
                    // Match with length slot
                    length_slot_ = number - 262;
                    if (length_slot_ < 8) {
                        length_lbits_ = 0;
                        cur_length_ = 2 + length_slot_;
                        state_ = state::MATCH_DECODE_DIST;
                    } else {
                        length_lbits_ = length_slot_ / 4 - 1;
                        cur_length_ = 2 + ((4 | (length_slot_ & 3)) << length_lbits_);
                        state_ = state::MATCH_LENGTH_EXTRA;
                    }
                }
                break;
            }

            case state::OUTPUT_LITERAL: {
                if (out_ptr >= out_end) {
                    return stream_result::need_output(bytes_read(), bytes_written());
                }

                *out_ptr++ = static_cast<u8>(cur_symbol_);
                window_[unp_ptr_++ & max_win_mask_] = static_cast<u8>(cur_symbol_);

                state_ = state::DECODE_SYMBOL;
                break;
            }

            case state::FILTER_READ_START_COUNT: {
                u32 byte_count = 0;
                if (!try_read_bits(in_ptr, in_end, 2, byte_count)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                vint_byte_count_ = byte_count + 1;
                vint_bytes_read_ = 0;
                vint_value_ = 0;
                state_ = state::FILTER_READ_START_BYTES;
                break;
            }

            case state::FILTER_READ_START_BYTES: {
                while (vint_bytes_read_ < vint_byte_count_) {
                    u32 byte_val = 0;
                    if (!try_read_bits(in_ptr, in_end, 8, byte_val)) {
                        if (input_finished) {
                            return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    vint_value_ += static_cast<u64>(byte_val) << (vint_bytes_read_ * 8);
                    vint_bytes_read_++;
                }
                filter_block_start_ = vint_value_;
                state_ = state::FILTER_READ_LENGTH_COUNT;
                break;
            }

            case state::FILTER_READ_LENGTH_COUNT: {
                u32 byte_count = 0;
                if (!try_read_bits(in_ptr, in_end, 2, byte_count)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                vint_byte_count_ = byte_count + 1;
                vint_bytes_read_ = 0;
                vint_value_ = 0;
                state_ = state::FILTER_READ_LENGTH_BYTES;
                break;
            }

            case state::FILTER_READ_LENGTH_BYTES: {
                while (vint_bytes_read_ < vint_byte_count_) {
                    u32 byte_val = 0;
                    if (!try_read_bits(in_ptr, in_end, 8, byte_val)) {
                        if (input_finished) {
                            return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    vint_value_ += static_cast<u64>(byte_val) << (vint_bytes_read_ * 8);
                    vint_bytes_read_++;
                }
                filter_block_length_ = vint_value_;
                state_ = state::FILTER_READ_TYPE;
                break;
            }

            case state::FILTER_READ_TYPE: {
                u32 filter_type = 0;
                if (!try_read_bits(in_ptr, in_end, 3, filter_type)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                filter_type_ = filter_type;

                if (filter_type_ == 0) {
                    // FILTER_DELTA needs channels
                    state_ = state::FILTER_READ_CHANNELS;
                } else if (filter_type_ <= 3) {
                    // FILTER_E8, FILTER_E8E9, FILTER_ARM
                    rar_filter filter;
                    filter.block_start = written_file_pos_ + filter_block_start_;
                    filter.block_length = filter_block_length_;
                    filter.channels = 0;

                    switch (filter_type_) {
                        case 1: filter.type = rar_filter_type::E8; break;
                        case 2: filter.type = rar_filter_type::E8E9; break;
                        case 3: filter.type = rar_filter_type::ARM; break;
                        default: break;
                    }

                    filter_processor_.add_filter(filter);
                    state_ = state::DECODE_SYMBOL;
                } else {
                    // Unknown filter - skip
                    state_ = state::DECODE_SYMBOL;
                }
                break;
            }

            case state::FILTER_READ_CHANNELS: {
                u32 channels = 0;
                if (!try_read_bits(in_ptr, in_end, 5, channels)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                rar_filter filter;
                filter.type = rar_filter_type::DELTA;
                filter.block_start = written_file_pos_ + filter_block_start_;
                filter.block_length = filter_block_length_;
                filter.channels = static_cast<u8>(channels + 1);

                filter_processor_.add_filter(filter);
                state_ = state::DECODE_SYMBOL;
                break;
            }

            case state::REPEAT_LAST: {
                if (last_length_ != 0) {
                    cur_length_ = last_length_;
                    cur_distance_ = old_dist_[0];
                    match_remaining_ = cur_length_;
                    state_ = state::COPY_MATCH;
                } else {
                    state_ = state::DECODE_SYMBOL;
                }
                break;
            }

            case state::SHORT_REPEAT_DECODE_LENGTH: {
                unsigned length_num = 0;
                if (!try_decode_number(in_ptr, in_end, tables_.rd, length_num)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                length_slot_ = length_num;
                if (length_slot_ < 8) {
                    length_lbits_ = 0;
                    cur_length_ = 2 + length_slot_;

                    // Reorder distances
                    for (unsigned i = dist_num_; i > 0; i--) {
                        old_dist_[i] = old_dist_[i - 1];
                    }
                    old_dist_[0] = cur_distance_;

                    last_length_ = cur_length_;
                    match_remaining_ = cur_length_;
                    state_ = state::COPY_MATCH;
                } else {
                    length_lbits_ = length_slot_ / 4 - 1;
                    cur_length_ = 2 + ((4 | (length_slot_ & 3)) << length_lbits_);
                    state_ = state::SHORT_REPEAT_LENGTH_EXTRA;
                }
                break;
            }

            case state::SHORT_REPEAT_LENGTH_EXTRA: {
                u32 extra = 0;
                if (!try_read_bits(in_ptr, in_end, length_lbits_, extra)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                cur_length_ += extra;

                // Reorder distances
                for (unsigned i = dist_num_; i > 0; i--) {
                    old_dist_[i] = old_dist_[i - 1];
                }
                old_dist_[0] = cur_distance_;

                last_length_ = cur_length_;
                match_remaining_ = cur_length_;
                state_ = state::COPY_MATCH;
                break;
            }

            case state::MATCH_LENGTH_EXTRA: {
                u32 extra = 0;
                if (!try_read_bits(in_ptr, in_end, length_lbits_, extra)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                cur_length_ += extra;
                state_ = state::MATCH_DECODE_DIST;
                break;
            }

            case state::MATCH_DECODE_DIST: {
                unsigned dist_slot = 0;
                if (!try_decode_number(in_ptr, in_end, tables_.dd, dist_slot)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                cur_dist_slot_ = dist_slot;
                cur_distance_ = 1;

                if (dist_slot < 4) {
                    cur_distance_ += dist_slot;
                    cur_d_bits_ = 0;
                } else {
                    cur_d_bits_ = dist_slot / 2 - 1;
                    cur_distance_ += size_t(2 | (dist_slot & 1)) << cur_d_bits_;
                }

                if (cur_d_bits_ > 0) {
                    if (cur_d_bits_ >= 4) {
                        if (cur_d_bits_ > 4) {
                            state_ = state::MATCH_DIST_EXTRA_HIGH;
                        } else {
                            state_ = state::MATCH_DIST_DECODE_LOW;
                        }
                    } else {
                        state_ = state::MATCH_DIST_EXTRA_LOW;
                    }
                } else {
                    // Add length adjustment
                    if (cur_distance_ > 0x100) {
                        cur_length_++;
                        if (cur_distance_ > 0x2000) {
                            cur_length_++;
                            if (cur_distance_ > 0x40000) {
                                cur_length_++;
                            }
                        }
                    }

                    insert_old_dist(cur_distance_);
                    last_length_ = cur_length_;
                    match_remaining_ = cur_length_;
                    state_ = state::COPY_MATCH;
                }
                break;
            }

            case state::MATCH_DIST_EXTRA_HIGH: {
                unsigned high_bits = cur_d_bits_ - 4;
                u32 extra = 0;
                if (!try_read_bits(in_ptr, in_end, high_bits, extra)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                cur_distance_ += static_cast<size_t>(extra) << 4;
                state_ = state::MATCH_DIST_DECODE_LOW;
                break;
            }

            case state::MATCH_DIST_DECODE_LOW: {
                unsigned low_dist = 0;
                if (!try_decode_number(in_ptr, in_end, tables_.ldd, low_dist)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                cur_distance_ += low_dist;

                // Add length adjustment
                if (cur_distance_ > 0x100) {
                    cur_length_++;
                    if (cur_distance_ > 0x2000) {
                        cur_length_++;
                        if (cur_distance_ > 0x40000) {
                            cur_length_++;
                        }
                    }
                }

                insert_old_dist(cur_distance_);
                last_length_ = cur_length_;
                match_remaining_ = cur_length_;
                state_ = state::COPY_MATCH;
                break;
            }

            case state::MATCH_DIST_EXTRA_LOW: {
                u32 extra = 0;
                if (!try_read_bits(in_ptr, in_end, cur_d_bits_, extra)) {
                    if (input_finished) {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                cur_distance_ += extra;

                // Add length adjustment
                if (cur_distance_ > 0x100) {
                    cur_length_++;
                    if (cur_distance_ > 0x2000) {
                        cur_length_++;
                        if (cur_distance_ > 0x40000) {
                            cur_length_++;
                        }
                    }
                }

                insert_old_dist(cur_distance_);
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
                    u8 value = window_[src_ptr & max_win_mask_];
                    *out_ptr++ = value;
                    window_[unp_ptr_++ & max_win_mask_] = value;
                    match_remaining_--;
                }

                state_ = state::DECODE_SYMBOL;
                break;
            }

            case state::CHECK_BLOCK_END: {
                if (block_header_.last_block) {
                    // Apply filters to output
                    size_t written = bytes_written();
                    filter_processor_.apply_filters(output.data(), written, written_file_pos_);
                    written_file_pos_ += written;
                    state_ = state::DONE;
                } else {
                    // More blocks to come
                    state_ = state::READ_BLOCK_HEADER_ALIGN;
                }
                break;
            }

            case state::DONE:
                break;
        }
    }

    size_t written = bytes_written();
    filter_processor_.apply_filters(output.data(), written, written_file_pos_);
    written_file_pos_ += written;

    return stream_result::done(bytes_read(), written);
}

}  // namespace crate
