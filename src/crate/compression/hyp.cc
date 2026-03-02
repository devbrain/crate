#include <crate/compression/hyp.hh>
#include <algorithm>

namespace crate {

hp_decompressor::hp_decompressor(size_t original_size, u8 version)
    : original_size_(original_size), version_(version) {
    init_state();
}

void hp_decompressor::reset() {
    init_state();
}

void hp_decompressor::init_state() {
    input_buffer_.clear();
    input_pos_ = 0;
    bit_cnt_ = 8;
    puffer_byte_ = 0;
    bytes_written_ = 0;

    // Allocate tables
    vater_.assign(2 * HUFF_SIZE + 2, 0);
    sohn_.assign(2 * HUFF_SIZE + 2, 0);
    the_freq_.assign(2 * HUFF_SIZE + 2, 0);
    nindex_.assign(STR_IND_BUF_LEN + 2, 0);
    nvalue_.assign(TAB_SIZE + 2, 0);
    frequencys_.assign(TAB_SIZE + 2, 0);
    nfreq_ = {};
    nfreqmax_.assign(MAX_REC_FREQUENCY + 1, 0);
    str_ind_buf_.assign(STR_IND_BUF_LEN + 1, 0);
    new_index_.assign(STR_IND_BUF_LEN + 1, 0);

    // Block state
    teststrings_index_ = 255;
    low_tsi_ = 2 * 255;
    lastposition_ = BASE_LASTPOSITION;
    huff_max_ = 2;
    huff_max_index_ = 2;
    maxlocal_ = 0;
    maxlocal255_ = 0;
    local_offset_ = 0;
    pos_offset_ = 0;
    char_offset_ = 0;
    mindiff_ = DEFAULT_MINDIFF;
    maxdiff_ = DEFAULT_MAXDIFF;

    // State machine
    state_ = state::BUFFERING_INPUT;
    rs_state_ = read_smart_state::INIT;
    rs_di_ = 0;
    rs_symbol_ = 0;
    rs_pos_ = 0;

    // decode_data state
    dd_si_ = 0;
    dd_ax_ = 0;
    dd_stack_.clear();
    dd_stack_.reserve(64);
    dd_need_next_entry_ = true;

    // tab_decode state
    td_in_progress_ = false;

    // Huffman decode state
    hd_in_progress_ = false;
}

// Bit reading
bool hp_decompressor::try_read_bit(bool& out) {
    if (bit_cnt_ == 8) {
        if (input_pos_ >= input_buffer_.size()) {
            return false;
        }
        puffer_byte_ = input_buffer_[input_pos_++];
    }

    out = (puffer_byte_ & 1) != 0;
    puffer_byte_ >>= 1;
    bit_cnt_--;
    if (bit_cnt_ == 0) {
        bit_cnt_ = 8;
    }
    return true;
}

bool hp_decompressor::try_read_bits(u8 count, u16& out) {
    // Save state for rollback
    size_t saved_pos = input_pos_;
    u8 saved_bit_cnt = bit_cnt_;
    u8 saved_puffer = puffer_byte_;

    u16 value = 0;
    for (u8 i = 0; i < count; i++) {
        bool bit;
        if (!try_read_bit(bit)) {
            // Rollback
            input_pos_ = saved_pos;
            bit_cnt_ = saved_bit_cnt;
            puffer_byte_ = saved_puffer;
            return false;
        }
        if (bit) {
            value |= (1u << i);
        }
    }
    out = value;
    return true;
}

// String buffer operations
u16 hp_decompressor::read_str(u16 offset) const {
    size_t idx = offset >> 1;
    return (idx < str_ind_buf_.size()) ? str_ind_buf_[idx] : 0;
}

void hp_decompressor::write_str(u16 offset, u16 value) {
    size_t idx = offset >> 1;
    if (idx < str_ind_buf_.size()) {
        str_ind_buf_[idx] = value;
    }
}

// Huffman tree operations
u16 hp_decompressor::get_sohn(u16 offset) const {
    size_t idx = offset >> 1;
    return (idx < sohn_.size()) ? sohn_[idx] : 0;
}

void hp_decompressor::set_sohn(u16 offset, u16 value) {
    size_t idx = offset >> 1;
    if (idx < sohn_.size()) sohn_[idx] = value;
}

u16 hp_decompressor::get_vater(u16 offset) const {
    size_t idx = offset >> 1;
    return (idx < vater_.size()) ? vater_[idx] : 0;
}

void hp_decompressor::set_vater(u16 offset, u16 value) {
    size_t idx = offset >> 1;
    if (idx < vater_.size()) vater_[idx] = value;
}

u16 hp_decompressor::get_the_freq(u16 offset) const {
    size_t idx = offset >> 1;
    return (idx < the_freq_.size()) ? the_freq_[idx] : 0;
}

void hp_decompressor::set_the_freq(u16 offset, u16 value) {
    size_t idx = offset >> 1;
    if (idx < the_freq_.size()) the_freq_[idx] = value;
}

u16 hp_decompressor::get_freq(u16 offset) const {
    size_t idx = offset >> 1;
    return (idx < frequencys_.size()) ? frequencys_[idx] : 0;
}

void hp_decompressor::set_freq(u16 offset, u16 value) {
    size_t idx = offset >> 1;
    if (idx < frequencys_.size()) frequencys_[idx] = value;
}

u16 hp_decompressor::get_nvalue(u16 offset) const {
    size_t idx = offset >> 1;
    return (idx < nvalue_.size()) ? nvalue_[idx] : 0;
}

void hp_decompressor::set_nvalue(u16 offset, u16 value) {
    size_t idx = offset >> 1;
    if (idx < nvalue_.size()) nvalue_[idx] = value;
}

void hp_decompressor::set_nindex(u16 offset, u16 value) {
    size_t idx = offset >> 1;
    if (idx < nindex_.size()) nindex_[idx] = value;
}

u16 hp_decompressor::get_nfreq(u16 freq_even) const {
    size_t slot = freq_even / 2;
    return (slot < NFREQ_TABLE_LEN) ? nfreq_[slot] : 0;
}

void hp_decompressor::set_nfreq(u16 freq_even, u16 value) {
    size_t slot = freq_even / 2;
    if (slot < NFREQ_TABLE_LEN) nfreq_[slot] = value;
}

u16 hp_decompressor::get_nfreqmax(u16 freq_even) const {
    if (freq_even < 2) return 0;
    size_t slot = (freq_even / 2) - 1;
    return (slot < nfreqmax_.size()) ? nfreqmax_[slot] : 0;
}

void hp_decompressor::set_nfreqmax(u16 freq_even, u16 value) {
    if (freq_even < 2) return;
    size_t slot = (freq_even / 2) - 1;
    if (slot < nfreqmax_.size()) nfreqmax_[slot] = value;
}

void hp_decompressor::markiere(u16 di, size_t& counter) {
    size_t idx = di >> 1;
    if (idx >= new_index_.size() || new_index_[idx] != 0) return;

    u16 si = read_str(di);
    if (si >= 512) {
        if (si >= 2) markiere(si - 2, counter);
        if (si != di) markiere(si, counter);
    }
    new_index_[idx] = MARKED;
    counter++;
}

void hp_decompressor::clear_when_full() {
    if (teststrings_index_ != STR_IND_BUF_LEN) return;

    std::fill(new_index_.begin(), new_index_.end(), 0);
    size_t count = 0;
    u16 di = static_cast<u16>(2 * STR_IND_BUF_LEN);
    while (di > 0) {
        di -= 2;
        markiere(di, count);
        if (count >= MAXMCOUNT || di == 0) break;
    }

    low_tsi_ = static_cast<u16>(std::min((count + 255) * 2, size_t(0xFFFF)));

    u16 bx = 2 * 255;
    u16 di_iter = 2 * 254;
    while (bx < low_tsi_) {
        di_iter += 2;
        size_t idx = di_iter >> 1;
        if (idx >= new_index_.size()) break;
        if (new_index_[idx] == 0) continue;

        new_index_[idx] = bx;
        u16 si = read_str(di_iter);
        if (si >= 512) {
            size_t map_idx = si >> 1;
            si = (map_idx < new_index_.size()) ? new_index_[map_idx] : 0;
        }

        write_str(bx, si);
        bx += 2;
    }
}

void hp_decompressor::set_vars() {
    u8 major = version_ >> 4;

    if (major == 3) {
        mindiff_ = -8;
        maxdiff_ = 8;
        maxlocal_ = 0x0096;
    } else {
        mindiff_ = -4;
        maxdiff_ = 8;

        i32 bx_i32 = static_cast<i16>(teststrings_index_ - 0x00ff);
        i32 prod = 0x009c * bx_i32;
        i32 quot = prod / 0x1f00;
        u16 ax_u16 = (static_cast<u16>(quot) & 0xfffe) + 4;
        maxlocal_ = ax_u16;
    }

    maxlocal255_ = maxlocal_ + 0x01fe;
    local_offset_ = static_cast<u16>(4 + maxdiff_ - mindiff_);
    pos_offset_ = static_cast<u16>(local_offset_ + maxlocal_ + 2);
    char_offset_ = pos_offset_ + 2 * (static_cast<u16>(MAX_FREQ) + 1);
}

void hp_decompressor::init_huff_tables() {
    std::fill(nfreqmax_.begin(), nfreqmax_.end(), 0);

    u16 di = char_offset_;
    u16 si = 0;
    for (size_t i = 0; i < STR_IND_BUF_LEN; i++) {
        size_t index = si >> 1;
        if (index >= nindex_.size()) break;
        nindex_[index] = di;

        size_t dest = di >> 1;
        if (dest < nvalue_.size()) {
            nvalue_[dest] = si;
            frequencys_[dest] = 0;
        }

        di += 2;
        si += 2;
    }

    for (auto& slot : nfreq_) {
        slot = char_offset_;
    }

    nfreq_[3] = 2;
    sohn_[0] = 1;
    the_freq_[0] = 2;
    vater_[0] = 0;
    huff_max_ = 2;
    huff_max_index_ = 2;
    frequencys_[0] = 0;

    u16 count = (char_offset_ / 2);
    if (count > 0) count--;
    while (count > 0) {
        ninsert();
        count--;
    }
}

void hp_decompressor::ninsert() {
    u16 di = huff_max_;
    u16 parent = di - 2;

    set_vater(di, parent);
    set_vater(di + 2, parent);
    set_the_freq(di + 2, 2);

    u16 leaf_marker = nfreq_[3] + 1;
    set_sohn(di + 2, leaf_marker);

    u16 old_child = get_sohn(parent);
    set_sohn(parent, di);
    set_sohn(di, old_child);

    if (old_child != 0) {
        set_freq(old_child - 1, di);
    }

    u16 parent_freq = get_the_freq(parent);
    set_the_freq(di, parent_freq);

    u16 leaf_slot = di + 2;
    set_freq(nfreq_[3], leaf_slot);

    nfreq_[3] += 2;
    huff_max_ = leaf_slot + 2;

    inc_frequency_ientry(parent);
}

void hp_decompressor::inc_frequency_ientry(u16 di) {
    u16 limit = 2 * MAX_REC_FREQUENCY;

    while (true) {
        u16 freq = get_the_freq(di);
        if (freq >= limit) return;

        u16 si = get_nfreqmax(freq);
        set_nfreqmax(freq, si + 2);

        if (di != si) {
            u16 bx_child = get_sohn(di);
            set_vater(bx_child, si);
            set_vater(bx_child + 2, si);
            inc_frequency_entry_swap(di, si, bx_child);
        }

        set_the_freq(si, get_the_freq(si) + 2);
        di = get_vater(si);
        if (si == 0) return;
    }
}

void hp_decompressor::inc_posfreq(u16 si) {
    u16 bx = get_freq(si);
    u16 di = get_nfreq(bx + 2);

    u16 item = get_nvalue(si);
    set_nindex(item, di);

    u16 swapped = get_nvalue(di);
    set_nvalue(di, item);
    item = swapped;

    set_nindex(item, si);
    set_nvalue(si, item);

    bx = get_freq(di);
    if (bx == 2 * MAX_FREQ) {
        ninsert();
        u16 di_next = huff_max_ - 2;
        inc_frequency(di_next);
        return;
    }

    bx += 2;
    set_freq(di, bx);
    di += 2;
    set_nfreq(bx, di);
}

void hp_decompressor::inc_frequency_entry_swap(u16 di, u16 si, u16 bx) {
    u16 old = get_sohn(si);
    set_sohn(si, bx);
    bx = old;

    if (bx & 1) {
        set_freq(bx - 1, di);
    } else {
        set_vater(bx, di);
        set_vater(bx + 2, di);
    }

    set_sohn(di, bx);
}

void hp_decompressor::inc_frequency(u16 di) {
    u16 limit = 2 * MAX_REC_FREQUENCY;

    while (true) {
        u16 freq = get_the_freq(di);
        if (freq >= limit) return;

        u16 si = get_nfreqmax(freq);
        set_nfreqmax(freq, si + 2);

        if (di != si) {
            u16 bx_child = get_sohn(di);
            if (bx_child & 1) {
                set_freq(bx_child - 1, si);
            } else {
                set_vater(bx_child, si);
                set_vater(bx_child + 2, si);
            }
            inc_frequency_entry_swap(di, si, bx_child);
        }

        set_the_freq(si, get_the_freq(si) + 2);
        di = get_vater(si);
        if (si == 0) return;
    }
}

bool hp_decompressor::try_decode_huff_entry(u16& out) {
    if (!hd_in_progress_) {
        hd_si_ = 0;
        hd_in_progress_ = true;
    }

    while (true) {
        u16 child = get_sohn(hd_si_);
        if (child & 1) {
            u16 leaf = child - 1;
            u16 freq_ptr = get_freq(leaf);
            inc_frequency(freq_ptr);
            out = leaf;
            hd_in_progress_ = false;
            return true;
        }

        bool bit;
        if (!try_read_bit(bit)) {
            return false;
        }
        hd_si_ = bit ? (child + 2) : child;
    }
}

bool hp_decompressor::try_tab_decode(u16 freq, u16& out) {
    if (!td_in_progress_) {
        td_base_ = freq - pos_offset_;
        td_ax_ = get_nfreq(td_base_);
        td_si_ = get_nfreq(td_base_ + 2);
        td_ax_ -= td_si_;
        td_ax_ >>= 1;
        td_dx_ = td_ax_;
        td_cx_ = 1;
        td_ax_ = 1;
        td_in_progress_ = true;
    }

    while (true) {
        bool bit;
        if (!try_read_bit(bit)) {
            return false;
        }
        if (!bit) {
            td_ax_ ^= td_cx_;
        }
        td_cx_ <<= 1;
        td_ax_ |= td_cx_;
        if (td_ax_ <= td_dx_) continue;
        td_ax_ ^= td_cx_;
        break;
    }

    td_ax_ <<= 1;
    td_si_ += td_ax_;
    u16 value = get_nvalue(td_si_);
    inc_posfreq(td_si_);
    out = value;
    td_in_progress_ = false;
    return true;
}

bool hp_decompressor::process_read_smart() {
    switch (rs_state_) {
        case read_smart_state::INIT: {
            u16 tsi;
            if (!try_read_bits(13, tsi)) {
                return false;
            }
            tsi &= 0x1fff;
            teststrings_index_ = tsi;
            set_vars();
            init_huff_tables();

            teststrings_index_ <<= 1;
            rs_di_ = low_tsi_ - 2;
            lastposition_ = BASE_LASTPOSITION;
            nfreq_[0] = char_offset_ + 2 * 255;
            rs_state_ = read_smart_state::DECODE_LOOP;
        }
        [[fallthrough]];

        case read_smart_state::DECODE_LOOP:
        decode_loop:
            while (true) {
                rs_di_ += 2;
                if (rs_di_ >= teststrings_index_) {
                    rs_state_ = read_smart_state::DONE;
                    return true;
                }

                if (rs_di_ > maxlocal255_) {
                    nfreq_[0] = static_cast<u16>(rs_di_ + char_offset_ - maxlocal_);
                }

                if (!try_decode_huff_entry(rs_symbol_)) {
                    rs_di_ -= 2;  // Will retry this iteration
                    return false;
                }

                if (rs_symbol_ == 2 * LSEQUENCE_KEY) {
                    rs_pos_ = lastposition_ + 4;
                    write_str(rs_di_, rs_pos_);
                    rs_di_ += 2;
                    rs_state_ = read_smart_state::LSEQUENCE_LOOP;
                    goto lsequence_loop;
                } else if (rs_symbol_ < local_offset_) {
                    u16 adjust = static_cast<u16>(2 * DIFF_OFFSET - mindiff_);
                    u16 new_pos = static_cast<u16>(lastposition_ + rs_symbol_ - adjust);
                    write_str(rs_di_, new_pos);
                    lastposition_ = new_pos;
                } else if (rs_symbol_ < pos_offset_) {
                    u16 offset = rs_symbol_ - local_offset_;
                    u16 new_pos = rs_di_ - offset;
                    write_str(rs_di_, new_pos);
                    lastposition_ = new_pos;
                } else if (rs_symbol_ < char_offset_) {
                    u16 value;
                    if (!try_tab_decode(rs_symbol_, value)) {
                        rs_di_ -= 2;  // Will retry
                        return false;
                    }
                    if (value >= 512) {
                        write_str(rs_di_, value);
                        lastposition_ = value;
                    } else {
                        write_str(rs_di_, value >> 1);
                    }
                } else {
                    u16 value = get_nvalue(rs_symbol_);
                    if (value >= 512) {
                        write_str(rs_di_, value);
                        lastposition_ = value;
                    } else {
                        write_str(rs_di_, value >> 1);
                    }
                }
            }

        case read_smart_state::LSEQUENCE_LOOP:
        lsequence_loop:
            while (true) {
                rs_pos_ += 4;
                write_str(rs_di_, rs_pos_);
                rs_di_ += 2;

                bool bit;
                if (!try_read_bit(bit)) {
                    rs_di_ -= 2;
                    rs_pos_ -= 4;
                    return false;
                }

                if (bit) continue;

                rs_di_ -= 2;
                lastposition_ = rs_pos_;
                rs_state_ = read_smart_state::DECODE_LOOP;
                break;
            }
            goto decode_loop;  // Continue decode loop after LSEQUENCE

        case read_smart_state::DONE:
            return true;
    }
    return true;
}

bool hp_decompressor::process_decode_data(byte*& out_ptr, byte* out_end) {
    u16 tsi_bytes = teststrings_index_;  // Already shifted in read_smart

    while (dd_si_ < tsi_bytes && bytes_written_ < original_size_) {
        if (dd_need_next_entry_) {
            dd_ax_ = read_str(dd_si_);
            dd_si_ += 2;
            dd_need_next_entry_ = false;
        }

        size_t steps = 0;
        while (true) {
            steps++;
            if (steps > 65536) {
                return true;  // Error, but let caller handle
            }

            if ((dd_ax_ & 0xFF00) == 0) {
                if (out_ptr >= out_end) {
                    return false;  // Need more output space
                }

                u8 value = static_cast<u8>(dd_ax_ & 0x00FF);
                *out_ptr++ = value;
                bytes_written_++;

                if (bytes_written_ >= original_size_) {
                    return true;
                }

                if (!dd_stack_.empty()) {
                    dd_ax_ = dd_stack_.back();
                    dd_stack_.pop_back();
                    continue;
                }
                dd_need_next_entry_ = true;
                break;
            } else {
                u16 bx = dd_ax_;
                u16 tail = read_str(bx);
                if (tail == bx) {
                    tail = read_str(bx - 2);
                }
                dd_stack_.push_back(tail);
                dd_ax_ = read_str(bx - 2);
            }
        }
    }

    return true;
}

result_t<stream_result> hp_decompressor::decompress_some(
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
    auto bytes_written_now = [&]() -> size_t {
        return static_cast<size_t>(out_ptr - output.data());
    };

    while (state_ != state::DONE) {
        switch (state_) {
            case state::BUFFERING_INPUT: {
                // Buffer all input data
                size_t to_copy = static_cast<size_t>(in_end - in_ptr);
                if (to_copy > 0) {
                    input_buffer_.insert(input_buffer_.end(), in_ptr, in_end);
                    in_ptr = in_end;
                }

                if (!input_finished) {
                    return stream_result::need_input(bytes_read(), bytes_written_now());
                }

                // Initialize bit reading from buffer
                // Note: Don't pre-load puffer_byte_, let try_read_bit do it
                // when bit_cnt_ == 8
                if (!input_buffer_.empty()) {
                    input_pos_ = 0;
                    bit_cnt_ = 8;  // Will trigger load on first try_read_bit
                    puffer_byte_ = 0;
                }

                state_ = state::READ_BLOCK_HEADER;
                break;
            }

            case state::READ_BLOCK_HEADER: {
                clear_when_full();
                rs_state_ = read_smart_state::INIT;
                state_ = state::READ_SMART;
                break;
            }

            case state::READ_SMART: {
                if (!process_read_smart()) {
                    // Shouldn't happen after buffering all input
                    return crate::make_unexpected(error{error_code::InputBufferUnderflow, "HYP: unexpected end of input"});
                }

                teststrings_index_ >>= 1;

                // Reset decode_data state
                dd_si_ = low_tsi_;
                dd_stack_.clear();
                dd_need_next_entry_ = true;
                teststrings_index_ <<= 1;  // Back to byte offset for decode

                state_ = state::DECODE_DATA;
                break;
            }

            case state::DECODE_DATA: {
                if (!process_decode_data(out_ptr, out_end)) {
                    // Need more output space
                    return stream_result::need_output(bytes_read(), bytes_written_now());
                }

                state_ = state::BLOCK_COMPLETE;
                break;
            }

            case state::BLOCK_COMPLETE: {
                teststrings_index_ >>= 1;

                // Update low_tsi
                u16 block_end = teststrings_index_ << 1;
                if (block_end >= 2 * 255) {
                    if (low_tsi_ > block_end) {
                        low_tsi_ = 2 * 255;
                    } else {
                        low_tsi_ = block_end;
                    }
                }

                // Check if done
                if (teststrings_index_ == 255 || bytes_written_ >= original_size_) {
                    state_ = state::DONE;
                    break;
                }

                // Check for end of input
                if (input_pos_ >= input_buffer_.size()) {
                    state_ = state::DONE;
                    break;
                }

                // More blocks to process
                state_ = state::READ_BLOCK_HEADER;
                break;
            }

            case state::DONE:
                break;
        }
    }

    return stream_result::done(bytes_read(), bytes_written_now());
}

} // namespace crate
