#include <crate/compression/ha.hh>
#include <algorithm>

namespace crate {

// =============================================================================
// Streaming Arithmetic Decoder
// =============================================================================

void ha_arithmetic_decoder::reset() {
    high_ = RANGE_MAX;
    low_ = 0;
    code_ = 0;
    byte_buffer_ = 0;
    bits_remaining_ = 0;
    bootstrapped_ = false;
}

bool ha_arithmetic_decoder::read_bit(const byte*& ptr, const byte* end, u16& out, bool end_of_stream) {
    if (bits_remaining_ == 0) {
        if (ptr >= end) {
            if (end_of_stream) {
                // Pad with zeros at end of stream
                byte_buffer_ = 0;
                bits_remaining_ = 8;
            } else {
                return false;
            }
        } else {
            byte_buffer_ = *ptr++;
            bits_remaining_ = 8;
        }
    }
    bits_remaining_--;
    out = (byte_buffer_ >> bits_remaining_) & 1;
    return true;
}

bool ha_arithmetic_decoder::renormalize(const byte*& ptr, const byte* end, bool end_of_stream) {
    while (true) {
        if ((high_ ^ low_) & MSB_MASK) {
            // MSBs don't match
            if ((low_ & UNDERFLOW_MASK) && !(high_ & UNDERFLOW_MASK)) {
                // E3 underflow: low = 01..., high = 10...
                low_ = (low_ << 1) & 0x7FFF;
                high_ = (high_ << 1) | 0x8001;
                u16 bit = 0;
                if (!read_bit(ptr, end, bit, end_of_stream)) {
                    return false;
                }
                code_ = ((code_ << 1) ^ MSB_MASK) | bit;
            } else {
                break;
            }
        } else {
            // MSBs match - shift out
            low_ <<= 1;
            high_ = (high_ << 1) | 1;
            u16 bit = 0;
            if (!read_bit(ptr, end, bit, end_of_stream)) {
                return false;
            }
            code_ = (code_ << 1) | bit;
        }
    }
    return true;
}

bool ha_arithmetic_decoder::try_threshold_val(const byte*& ptr, const byte* end, u16 total, u16& out, bool end_of_stream) {
    // Bootstrap: read initial 16-bit code value (big-endian)
    if (!bootstrapped_) {
        if (ptr + 2 > end) {
            if (end_of_stream) {
                // Pad with zeros if at end of stream
                if (ptr + 1 == end) {
                    code_ = static_cast<u16>(ptr[0]) << 8;
                    ptr++;
                } else {
                    code_ = 0;
                }
                bootstrapped_ = true;
            } else {
                return false;
            }
        } else {
            code_ = (static_cast<u16>(ptr[0]) << 8) | ptr[1];
            ptr += 2;
            bootstrapped_ = true;
        }
    }

    u32 range = static_cast<u32>(high_ - low_) + 1;
    u32 offset = static_cast<u32>(code_ - low_) + 1;
    out = static_cast<u16>((offset * total - 1) / range);
    return true;
}

bool ha_arithmetic_decoder::try_decode_update(const byte*& ptr, const byte* end,
                                              u16 cum_low, u16 cum_high, u16 total, bool end_of_stream) {
    u32 range = static_cast<u32>(high_ - low_) + 1;
    u32 scale = total;

    u16 new_high = low_ + static_cast<u16>((range * cum_high / scale) - 1);
    u16 new_low = low_ + static_cast<u16>(range * cum_low / scale);

    high_ = new_high;
    low_ = new_low;

    return renormalize(ptr, end, end_of_stream);
}

// =============================================================================
// Binary Tree Frequency Table
// =============================================================================

ha_binary_tree_table::ha_binary_tree_table(size_t leaf_count, u16 initial_value)
    : leaf_count_(leaf_count), storage_(leaf_count * 2, 0) {
    for (size_t i = leaf_count_; i < 2 * leaf_count_; i++) {
        storage_[i] = initial_value;
    }
    recompute_internals();
}

void ha_binary_tree_table::init(size_t leaf_count, u16 initial_value) {
    leaf_count_ = leaf_count;
    storage_.assign(leaf_count * 2, 0);
    for (size_t i = leaf_count_; i < 2 * leaf_count_; i++) {
        storage_[i] = initial_value;
    }
    recompute_internals();
}

u16 ha_binary_tree_table::symbol_freq(size_t symbol) const {
    return storage_[leaf_count_ + symbol];
}

std::pair<size_t, u16> ha_binary_tree_table::navigate_to_symbol(u16 threshold) const {
    size_t node = 2;
    u16 cumulative = 0;

    while (true) {
        u16 left_sum = storage_[node];
        if (cumulative + left_sum <= threshold) {
            cumulative += left_sum;
            node++;
        }
        if (node >= leaf_count_) {
            return {node - leaf_count_, cumulative};
        }
        node <<= 1;
    }
}

void ha_binary_tree_table::add_frequency(size_t symbol, u16 step, u16 max_total) {
    size_t idx = symbol + leaf_count_;
    while (idx > 0) {
        storage_[idx] += step;
        idx >>= 1;
    }
    if (storage_[1] >= max_total) {
        halve_all();
    }
}

void ha_binary_tree_table::remove_symbol(size_t symbol) {
    size_t idx = symbol + leaf_count_;
    u16 amount = storage_[idx];
    while (idx > 0) {
        storage_[idx] -= amount;
        idx >>= 1;
    }
}

void ha_binary_tree_table::recompute_internals() {
    size_t src = (leaf_count_ << 1) - 2;
    for (size_t dest = leaf_count_ - 1; dest >= 1; dest--) {
        storage_[dest] = storage_[src] + storage_[src + 1];
        src -= 2;
    }
}

void ha_binary_tree_table::halve_all() {
    for (size_t i = leaf_count_; i < 2 * leaf_count_; i++) {
        if (storage_[i] > 1) {
            storage_[i] >>= 1;
        }
    }
    recompute_internals();
}

// =============================================================================
// ASC Decompressor - Streaming Implementation
// =============================================================================

ha_asc_decompressor::ha_asc_decompressor() {
    init_state();
}

void ha_asc_decompressor::reset() {
    init_state();
}

void ha_asc_decompressor::init_state() {
    coder_.reset();

    char_main_.init(CHAR_TABLE_SIZE, 0);
    char_escape_.init(CHAR_TABLE_SIZE, 1);
    len_main_.init(LEN_TABLE_SIZE, 0);
    len_escape_.init(LEN_TABLE_SIZE, 1);
    pos_table_.init(POS_TABLE_SIZE, 0);

    for (auto& ctx : type_frequencies_) {
        ctx[0] = TYPE_STEP;
        ctx[1] = TYPE_STEP;
    }
    pos_table_.add_frequency(0, POS_STEP, TOTAL_MAX);

    type_context_ = 0;
    char_escape_weight_ = 1;
    len_escape_weight_ = LEN_STEP;
    pos_codes_active_ = 1;
    pos_max_value_ = 1;
    bytes_emitted_ = 0;

    window_.assign(WINDOW_CAPACITY, 0);
    write_pos_ = 0;

    state_ = state::DECODE_TYPE;
    phase_ = decode_phase::THRESHOLD;

    decoded_symbol_ = 0;
    match_position_ = 0;
    match_length_ = 0;
    match_remaining_ = 0;
}

void ha_asc_decompressor::record_literal() {
    u16 total = type_frequencies_[type_context_][0] + type_frequencies_[type_context_][1];
    type_frequencies_[type_context_][0] += TYPE_STEP;
    if (total >= TOTAL_MAX) {
        scale_type_context();
    }
    type_context_ = (type_context_ << 1) & 0x3;
}

void ha_asc_decompressor::record_match() {
    u16 total = type_frequencies_[type_context_][0] + type_frequencies_[type_context_][1];
    type_frequencies_[type_context_][1] += TYPE_STEP;
    if (total >= TOTAL_MAX) {
        scale_type_context();
    }
    type_context_ = ((type_context_ << 1) | 1) & 0x3;
}

void ha_asc_decompressor::scale_type_context() {
    type_frequencies_[type_context_][0] = std::max(
        u16(1), u16(type_frequencies_[type_context_][0] >> 1));
    type_frequencies_[type_context_][1] = std::max(
        u16(1), u16(type_frequencies_[type_context_][1] >> 1));
}

result_t<stream_result> ha_asc_decompressor::decompress_some(
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

    while (state_ != state::DONE) {
        switch (state_) {
            case state::DECODE_TYPE: {
                // Get type_total and threshold
                u16 type_total = type_frequencies_[type_context_][0] + type_frequencies_[type_context_][1];

                if (phase_ == decode_phase::THRESHOLD) {
                    if (!coder_.try_threshold_val(in_ptr, in_end, type_total + 1, threshold_, input_finished)) {
                        if (input_finished) {
                            state_ = state::DONE;
                            return stream_result::done(bytes_read(), bytes_written());
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    phase_ = decode_phase::MAIN_UPDATE;
                }

                u16 lit_freq = type_frequencies_[type_context_][0];

                if (lit_freq > threshold_) {
                    // Literal
                    if (!coder_.try_decode_update(in_ptr, in_end, 0, lit_freq, type_total + 1, input_finished)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    record_literal();
                    phase_ = decode_phase::THRESHOLD;
                    state_ = state::DECODE_LITERAL;
                } else if (type_total > threshold_) {
                    // Match
                    if (!coder_.try_decode_update(in_ptr, in_end, lit_freq, type_total, type_total + 1, input_finished)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    record_match();
                    phase_ = decode_phase::THRESHOLD;
                    state_ = state::EXPAND_POS_TABLE;
                } else {
                    // End of stream
                    if (!coder_.try_decode_update(in_ptr, in_end, type_total, type_total + 1, type_total + 1, input_finished)) {
                        if (input_finished) {
                            // OK to end here
                            state_ = state::DONE;
                            return stream_result::done(bytes_read(), bytes_written());
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    state_ = state::DONE;
                }
                break;
            }

            case state::DECODE_LITERAL: {
                // Decode character using char_main_ or char_escape_
                main_total_ = char_main_.root_sum();
                combined_total_ = main_total_ + char_escape_weight_;

                if (phase_ == decode_phase::THRESHOLD) {
                    if (!coder_.try_threshold_val(in_ptr, in_end, combined_total_, threshold_, input_finished)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    phase_ = decode_phase::MAIN_NAVIGATE;
                }

                if (threshold_ >= main_total_) {
                    // Escape to new character
                    if (phase_ == decode_phase::MAIN_NAVIGATE) {
                        if (!coder_.try_decode_update(in_ptr, in_end, main_total_, combined_total_, combined_total_, input_finished)) {
                            if (input_finished) {
                                return std::unexpected(error{error_code::InputBufferUnderflow});
                            }
                            return stream_result::need_input(bytes_read(), bytes_written());
                        }
                        phase_ = decode_phase::ESCAPE_UPDATE;
                    }

                    if (phase_ == decode_phase::ESCAPE_UPDATE) {
                        u16 esc_total = char_escape_.root_sum();
                        if (!coder_.try_threshold_val(in_ptr, in_end, esc_total, threshold_, input_finished)) {
                            if (input_finished) {
                                return std::unexpected(error{error_code::InputBufferUnderflow});
                            }
                            return stream_result::need_input(bytes_read(), bytes_written());
                        }
                        auto [sym, lt] = char_escape_.navigate_to_symbol(threshold_);
                        nav_symbol_ = sym;
                        nav_lt_ = lt;
                        nav_freq_ = char_escape_.symbol_freq(sym);
                        phase_ = decode_phase::ESCAPE_SYMBOL_UPDATE;
                    }

                    if (phase_ == decode_phase::ESCAPE_SYMBOL_UPDATE) {
                        u16 esc_total = char_escape_.root_sum();
                        if (!coder_.try_decode_update(in_ptr, in_end, nav_lt_, nav_lt_ + nav_freq_, esc_total, input_finished)) {
                            if (input_finished) {
                                return std::unexpected(error{error_code::InputBufferUnderflow});
                            }
                            return stream_result::need_input(bytes_read(), bytes_written());
                        }

                        char_escape_.remove_symbol(nav_symbol_);
                        if (char_escape_.root_sum() != 0) {
                            char_escape_weight_++;
                        } else {
                            char_escape_weight_ = 0;
                        }

                        // Boost locality
                        size_t start = (nav_symbol_ > CHAR_LOCALITY) ? nav_symbol_ - CHAR_LOCALITY : 0;
                        size_t end = std::min(nav_symbol_ + CHAR_LOCALITY, CHAR_TABLE_SIZE - 1);
                        for (size_t i = start; i < end; i++) {
                            if (char_escape_.symbol_freq(i) > 0) {
                                char_escape_.add_frequency(i, 1, CHAR_FREQ_MAX);
                            }
                        }
                        decoded_symbol_ = static_cast<u16>(nav_symbol_);
                        phase_ = decode_phase::FINAL_UPDATE;
                    }
                } else {
                    // Decode from main table
                    if (phase_ == decode_phase::MAIN_NAVIGATE) {
                        auto [sym, lt] = char_main_.navigate_to_symbol(threshold_);
                        nav_symbol_ = sym;
                        nav_lt_ = lt;
                        nav_freq_ = char_main_.symbol_freq(sym);
                        phase_ = decode_phase::MAIN_UPDATE;
                    }

                    if (phase_ == decode_phase::MAIN_UPDATE) {
                        if (!coder_.try_decode_update(in_ptr, in_end, nav_lt_, nav_lt_ + nav_freq_, combined_total_, input_finished)) {
                            if (input_finished) {
                                return std::unexpected(error{error_code::InputBufferUnderflow});
                            }
                            return stream_result::need_input(bytes_read(), bytes_written());
                        }
                        decoded_symbol_ = static_cast<u16>(nav_symbol_);
                        phase_ = decode_phase::FINAL_UPDATE;
                    }
                }

                if (phase_ == decode_phase::FINAL_UPDATE) {
                    char_main_.add_frequency(decoded_symbol_, 1, CHAR_FREQ_MAX);

                    if (char_main_.symbol_freq(decoded_symbol_) == 3) {
                        if (char_escape_weight_ > 1) {
                            char_escape_weight_--;
                        }
                    }

                    // Output byte
                    if (out_ptr >= out_end) {
                        return stream_result::need_output(bytes_read(), bytes_written());
                    }

                    u8 decoded_byte = static_cast<u8>(decoded_symbol_);
                    *out_ptr++ = decoded_byte;
                    window_[write_pos_++] = decoded_byte;
                    if (write_pos_ >= WINDOW_CAPACITY) write_pos_ = 0;

                    if (bytes_emitted_ < WINDOW_CAPACITY) bytes_emitted_++;

                    phase_ = decode_phase::THRESHOLD;
                    state_ = state::DECODE_TYPE;
                }
                break;
            }

            case state::EXPAND_POS_TABLE: {
                // Expand position table if needed
                while (bytes_emitted_ > pos_max_value_) {
                    pos_table_.add_frequency(pos_codes_active_, POS_STEP, TOTAL_MAX);
                    pos_codes_active_++;
                    pos_max_value_ <<= 1;
                }
                phase_ = decode_phase::THRESHOLD;
                state_ = state::DECODE_POSITION;
                break;
            }

            case state::DECODE_POSITION: {
                // Decode position code
                main_total_ = pos_table_.root_sum();

                if (phase_ == decode_phase::THRESHOLD) {
                    if (!coder_.try_threshold_val(in_ptr, in_end, main_total_, threshold_, input_finished)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    auto [code, lt] = pos_table_.navigate_to_symbol(threshold_);
                    code_value_ = code;
                    nav_lt_ = lt;
                    nav_freq_ = pos_table_.symbol_freq(code);
                    phase_ = decode_phase::MAIN_UPDATE;
                }

                if (phase_ == decode_phase::MAIN_UPDATE) {
                    if (!coder_.try_decode_update(in_ptr, in_end, nav_lt_, nav_lt_ + nav_freq_, main_total_, input_finished)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }

                    pos_table_.add_frequency(code_value_, POS_STEP, TOTAL_MAX);

                    if (code_value_ > 1) {
                        phase_ = decode_phase::THRESHOLD;
                        state_ = state::DECODE_POS_EXTRA;
                    } else {
                        match_position_ = static_cast<u16>(code_value_);
                        phase_ = decode_phase::THRESHOLD;
                        state_ = state::DECODE_LENGTH;
                    }
                }
                break;
            }

            case state::DECODE_POS_EXTRA: {
                // Decode extra bits for position
                u16 base = 1u << (code_value_ - 1);
                u16 range = (base == (pos_max_value_ >> 1))
                                ? bytes_emitted_ - (pos_max_value_ >> 1)
                                : base;

                if (phase_ == decode_phase::THRESHOLD) {
                    if (!coder_.try_threshold_val(in_ptr, in_end, range, threshold_, input_finished)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    phase_ = decode_phase::MAIN_UPDATE;
                }

                if (phase_ == decode_phase::MAIN_UPDATE) {
                    if (!coder_.try_decode_update(in_ptr, in_end, threshold_, threshold_ + 1, range, input_finished)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    match_position_ = threshold_ + base;
                    phase_ = decode_phase::THRESHOLD;
                    state_ = state::DECODE_LENGTH;
                }
                break;
            }

            case state::DECODE_LENGTH: {
                // Decode length using len_main_ or len_escape_
                main_total_ = len_main_.root_sum();
                combined_total_ = main_total_ + len_escape_weight_;

                if (phase_ == decode_phase::THRESHOLD) {
                    if (!coder_.try_threshold_val(in_ptr, in_end, combined_total_, threshold_, input_finished)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    phase_ = decode_phase::MAIN_NAVIGATE;
                }

                if (threshold_ >= main_total_) {
                    // Escape
                    if (phase_ == decode_phase::MAIN_NAVIGATE) {
                        if (!coder_.try_decode_update(in_ptr, in_end, main_total_, combined_total_, combined_total_, input_finished)) {
                            if (input_finished) {
                                return std::unexpected(error{error_code::InputBufferUnderflow});
                            }
                            return stream_result::need_input(bytes_read(), bytes_written());
                        }
                        phase_ = decode_phase::ESCAPE_UPDATE;
                    }

                    if (phase_ == decode_phase::ESCAPE_UPDATE) {
                        u16 esc_total = len_escape_.root_sum();
                        if (!coder_.try_threshold_val(in_ptr, in_end, esc_total, threshold_, input_finished)) {
                            if (input_finished) {
                                return std::unexpected(error{error_code::InputBufferUnderflow});
                            }
                            return stream_result::need_input(bytes_read(), bytes_written());
                        }
                        auto [sym, lt] = len_escape_.navigate_to_symbol(threshold_);
                        nav_symbol_ = sym;
                        nav_lt_ = lt;
                        nav_freq_ = len_escape_.symbol_freq(sym);
                        phase_ = decode_phase::ESCAPE_SYMBOL_UPDATE;
                    }

                    if (phase_ == decode_phase::ESCAPE_SYMBOL_UPDATE) {
                        u16 esc_total = len_escape_.root_sum();
                        if (!coder_.try_decode_update(in_ptr, in_end, nav_lt_, nav_lt_ + nav_freq_, esc_total, input_finished)) {
                            if (input_finished) {
                                return std::unexpected(error{error_code::InputBufferUnderflow});
                            }
                            return stream_result::need_input(bytes_read(), bytes_written());
                        }

                        len_escape_.remove_symbol(nav_symbol_);
                        if (len_escape_.root_sum() != 0) {
                            len_escape_weight_ += LEN_STEP;
                        } else {
                            len_escape_weight_ = 0;
                        }

                        // Boost locality
                        size_t start = (nav_symbol_ > LEN_LOCALITY) ? nav_symbol_ - LEN_LOCALITY : 0;
                        size_t end = std::min(nav_symbol_ + LEN_LOCALITY, LEN_TABLE_SIZE - 1);
                        for (size_t i = start; i < end; i++) {
                            if (len_escape_.symbol_freq(i) > 0) {
                                len_escape_.add_frequency(i, 1, TOTAL_MAX);
                            }
                        }
                        code_value_ = nav_symbol_;
                        phase_ = decode_phase::FINAL_UPDATE;
                    }
                } else {
                    // Decode from main table
                    if (phase_ == decode_phase::MAIN_NAVIGATE) {
                        auto [sym, lt] = len_main_.navigate_to_symbol(threshold_);
                        nav_symbol_ = sym;
                        nav_lt_ = lt;
                        nav_freq_ = len_main_.symbol_freq(sym);
                        phase_ = decode_phase::MAIN_UPDATE;
                    }

                    if (phase_ == decode_phase::MAIN_UPDATE) {
                        if (!coder_.try_decode_update(in_ptr, in_end, nav_lt_, nav_lt_ + nav_freq_, combined_total_, input_finished)) {
                            if (input_finished) {
                                return std::unexpected(error{error_code::InputBufferUnderflow});
                            }
                            return stream_result::need_input(bytes_read(), bytes_written());
                        }
                        code_value_ = nav_symbol_;
                        phase_ = decode_phase::FINAL_UPDATE;
                    }
                }

                if (phase_ == decode_phase::FINAL_UPDATE) {
                    len_main_.add_frequency(code_value_, LEN_STEP, TOTAL_MAX);

                    if (len_main_.symbol_freq(code_value_) == 3 * LEN_STEP) {
                        if (len_escape_weight_ > LEN_STEP) {
                            len_escape_weight_ -= LEN_STEP;
                        }
                    }

                    // Compute raw_length
                    if (code_value_ == SHORT_LEN_COUNT - 1) {
                        raw_length_ = TOTAL_LENGTHS - 1;
                        match_length_ = raw_length_ + MIN_MATCH;
                        match_remaining_ = match_length_;
                        phase_ = decode_phase::THRESHOLD;
                        state_ = state::COPY_MATCH;
                    } else if (code_value_ >= SHORT_LEN_COUNT) {
                        // Need extra bits
                        phase_ = decode_phase::THRESHOLD;
                        state_ = state::DECODE_LEN_EXTRA;
                    } else {
                        raw_length_ = static_cast<u16>(code_value_);
                        match_length_ = raw_length_ + MIN_MATCH;
                        match_remaining_ = match_length_;
                        phase_ = decode_phase::THRESHOLD;
                        state_ = state::COPY_MATCH;
                    }
                }
                break;
            }

            case state::DECODE_LEN_EXTRA: {
                // Decode extra bits for long length
                if (phase_ == decode_phase::THRESHOLD) {
                    if (!coder_.try_threshold_val(in_ptr, in_end, LONG_LEN_RANGE, threshold_, input_finished)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    phase_ = decode_phase::MAIN_UPDATE;
                }

                if (phase_ == decode_phase::MAIN_UPDATE) {
                    if (!coder_.try_decode_update(in_ptr, in_end, threshold_, threshold_ + 1, LONG_LEN_RANGE, input_finished)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }

                    u32 base = static_cast<u32>(code_value_ - SHORT_LEN_COUNT);
                    u32 computed = (base << LONG_LEN_BITS) + threshold_ + SHORT_LEN_COUNT - 1;
                    raw_length_ = static_cast<u16>(computed);
                    match_length_ = raw_length_ + MIN_MATCH;
                    match_remaining_ = match_length_;
                    phase_ = decode_phase::THRESHOLD;
                    state_ = state::COPY_MATCH;
                }
                break;
            }

            case state::COPY_MATCH: {
                // Copy bytes from window
                size_t capacity = window_.size();

                while (match_remaining_ > 0) {
                    if (out_ptr >= out_end) {
                        return stream_result::need_output(bytes_read(), bytes_written());
                    }

                    size_t read_pos = (write_pos_ > match_position_)
                                          ? write_pos_ - 1 - match_position_
                                          : capacity - 1 - match_position_ + write_pos_;

                    u8 match_byte = window_[read_pos];
                    *out_ptr++ = match_byte;
                    window_[write_pos_] = match_byte;

                    write_pos_++;
                    if (write_pos_ >= capacity) write_pos_ = 0;

                    match_remaining_--;
                }

                if (bytes_emitted_ < WINDOW_CAPACITY) {
                    bytes_emitted_ += match_length_;
                    if (bytes_emitted_ > WINDOW_CAPACITY) {
                        bytes_emitted_ = WINDOW_CAPACITY;
                    }
                }

                phase_ = decode_phase::THRESHOLD;
                state_ = state::DECODE_TYPE;
                break;
            }

            case state::DONE:
                break;
        }
    }

    return stream_result::done(bytes_read(), bytes_written());
}

// =============================================================================
// HSC Decompressor - Streaming Implementation
// =============================================================================

ha_hsc_decompressor::ha_hsc_decompressor() {
    init_state();
}

void ha_hsc_decompressor::reset() {
    init_state();
}

void ha_hsc_decompressor::init_state() {
    coder_.reset();

    // Initialize hash table
    hash_heads_.assign(HASH_TABLE_SIZE, NULL_PTR);
    hash_chain_.assign(CONTEXT_POOL_SIZE, 0);

    // Initialize LRU list
    lru_prev_.resize(CONTEXT_POOL_SIZE);
    lru_next_.resize(CONTEXT_POOL_SIZE);
    for (size_t i = 0; i < CONTEXT_POOL_SIZE; i++) {
        lru_next_[i] = static_cast<u16>(i + 1);
        lru_prev_[i] = static_cast<u16>(i - 1);
    }
    lru_front_ = 0;
    lru_back_ = static_cast<u16>(CONTEXT_POOL_SIZE - 1);

    // Initialize context pool
    ctx_bytes_.resize(CONTEXT_POOL_SIZE);
    ctx_length_.assign(CONTEXT_POOL_SIZE, 0xFF);
    ctx_char_count_.assign(CONTEXT_POOL_SIZE, 0);
    ctx_total_freq_.assign(CONTEXT_POOL_SIZE, 0);
    ctx_low_freq_count_.assign(CONTEXT_POOL_SIZE, 0);
    ctx_rescale_factor_.assign(CONTEXT_POOL_SIZE, 0);

    // Initialize frequency block pool
    freq_value_.assign(FREQ_BLOCK_POOL_SIZE, 0);
    freq_char_.assign(FREQ_BLOCK_POOL_SIZE, 0);
    freq_next_.assign(FREQ_BLOCK_POOL_SIZE, NULL_PTR);

    for (size_t i = CONTEXT_POOL_SIZE; i < FREQ_BLOCK_POOL_SIZE - 1; i++) {
        freq_next_[i] = static_cast<u16>(i + 1);
    }
    free_block_head_ = static_cast<u16>(CONTEXT_POOL_SIZE);

    // Build hash randomization table
    build_hash_table();

    // Initialize escape tracking
    initial_escape_[0] = ESCAPE_COUNTER_LIMIT >> 1;
    for (size_t i = 1; i <= MAX_ORDER; i++) {
        initial_escape_[i] = (ESCAPE_COUNTER_LIMIT >> 1) - 1;
    }

    order_reduction_counter_ = static_cast<i16>(CONTEXT_POOL_SIZE / 4);
    current_max_order_ = static_cast<u8>(MAX_ORDER);

    context_window_ = {};
    excluded_ = {};
    excluded_stack_.clear();
    non_escape_count_ = 0;
    update_depth_ = 0;
    reclaim_cursor_ = 0;

    state_ = state::FIND_CONTEXT;
    phase_ = decode_phase::THRESHOLD;
}

void ha_hsc_decompressor::build_hash_table() {
    hash_rand_.resize(HASH_TABLE_SIZE);
    i64 seed = 10;

    for (size_t i = 0; i < HASH_TABLE_SIZE; i++) {
        i64 quotient = seed / (2147483647LL / 16807);
        i64 remainder = seed % (2147483647LL / 16807);
        i64 product = 16807LL * remainder - (2147483647LL % 16807) * quotient;
        seed = (product > 0) ? product : product + 2147483647LL;
        hash_rand_[i] = static_cast<u16>(seed) & static_cast<u16>(HASH_TABLE_SIZE - 1);
    }
}

u16 ha_hsc_decompressor::compute_hash(const std::array<u8, 4>& bytes, size_t length) const {
    u16 mask = static_cast<u16>(HASH_TABLE_SIZE - 1);
    u16 h = 0;
    for (size_t i = 0; i < std::min(length, size_t(4)); i++) {
        h = hash_rand_[((bytes[i] + h) & mask)];
    }
    return h;
}

void ha_hsc_decompressor::advance_context(u8 ctx_byte) {
    context_window_[3] = context_window_[2];
    context_window_[2] = context_window_[1];
    context_window_[1] = context_window_[0];
    context_window_[0] = ctx_byte;
}

void ha_hsc_decompressor::promote_to_front(u16 ctx_id) {
    if (ctx_id == lru_front_) return;

    size_t idx = ctx_id;

    if (ctx_id == lru_back_) {
        lru_back_ = lru_prev_[idx];
    } else {
        u16 next = lru_next_[idx];
        u16 prev = lru_prev_[idx];
        lru_prev_[next] = prev;
        lru_next_[prev] = next;
    }

    lru_prev_[lru_front_] = ctx_id;
    lru_next_[idx] = lru_front_;
    lru_front_ = ctx_id;
}

void ha_hsc_decompressor::prepare_context_search() {
    u16 mask = static_cast<u16>(HASH_TABLE_SIZE - 1);

    order_hashes_[1] = hash_rand_[context_window_[0]];
    order_hashes_[2] = hash_rand_[(context_window_[1] + order_hashes_[1]) & mask];
    order_hashes_[3] = hash_rand_[(context_window_[2] + order_hashes_[2]) & mask];
    order_hashes_[4] = hash_rand_[(context_window_[3] + order_hashes_[3]) & mask];

    update_depth_ = 0;
    excluded_stack_.clear();
    std::fill(excluded_.begin(), excluded_.end(), false);
    search_order_ = static_cast<i16>(MAX_ORDER + 1);
}

u16 ha_hsc_decompressor::find_longest_context() {
    prepare_context_search();
    return find_next_context();
}

u16 ha_hsc_decompressor::find_next_context() {
    for (int order = search_order_ - 1; order >= 0; order--) {
        u16 hash = order_hashes_[static_cast<size_t>(order)];
        u16 ctx_id = hash_heads_[hash];

        while (ctx_id != NULL_PTR) {
            size_t idx = ctx_id;

            if (ctx_length_[idx] == order && context_matches(idx, order)) {
                search_order_ = static_cast<i16>(order);
                return ctx_id;
            }

            ctx_id = hash_chain_[idx];
        }
    }
    return NULL_PTR;
}

bool ha_hsc_decompressor::context_matches(size_t idx, int order) const {
    switch (order) {
        case 4: return ctx_bytes_[idx][0] == context_window_[0] &&
                       ctx_bytes_[idx][1] == context_window_[1] &&
                       ctx_bytes_[idx][2] == context_window_[2] &&
                       ctx_bytes_[idx][3] == context_window_[3];
        case 3: return ctx_bytes_[idx][0] == context_window_[0] &&
                       ctx_bytes_[idx][1] == context_window_[1] &&
                       ctx_bytes_[idx][2] == context_window_[2];
        case 2: return ctx_bytes_[idx][0] == context_window_[0] &&
                       ctx_bytes_[idx][1] == context_window_[1];
        case 1: return ctx_bytes_[idx][0] == context_window_[0];
        case 0: return true;
        default: return false;
    }
}

u16 ha_hsc_decompressor::calculate_escape_probability(u16 low_freq_count, u16 ctx_id) const {
    size_t idx = ctx_id;
    u16 total = ctx_total_freq_[idx];
    u8 char_count = ctx_char_count_[idx];

    if (total == 1) {
        return (initial_escape_[ctx_length_[idx]] >= (ESCAPE_COUNTER_LIMIT >> 1)) ? 2 : 1;
    }

    if (char_count == 255) return 1;

    u16 escape = low_freq_count;

    if (char_count > 0 && ((static_cast<u16>(char_count) + 1) << 1) >= total) {
        escape = static_cast<u16>(
            (static_cast<u32>(escape) * ((static_cast<u32>(char_count) + 1) << 1)) / total);

        if (static_cast<u16>(char_count) + 1 == total) {
            escape = static_cast<u16>(escape + ((static_cast<u16>(char_count) + 1) >> 1));
        }
    }

    return std::max(static_cast<u16>(1), escape);
}

void ha_hsc_decompressor::reclaim_blocks() {
    while (true) {
        while (true) {
            reclaim_cursor_++;
            if (reclaim_cursor_ >= CONTEXT_POOL_SIZE) reclaim_cursor_ = 0;
            if (freq_next_[reclaim_cursor_] != NULL_PTR) break;
        }

        bool in_stack = false;
        for (size_t i = 0; i <= update_depth_; i++) {
            if ((update_contexts_[i] & 0x7FFF) == reclaim_cursor_) {
                in_stack = true;
                break;
            }
        }

        if (!in_stack) break;
    }

    size_t ctx = reclaim_cursor_;

    u16 min_freq = freq_value_[ctx];
    u16 blk = freq_next_[ctx];
    while (blk != NULL_PTR) {
        if (freq_value_[blk] < min_freq) min_freq = freq_value_[blk];
        blk = freq_next_[blk];
    }
    min_freq++;

    if (freq_value_[ctx] < min_freq) {
        blk = freq_next_[ctx];
        while (freq_value_[blk] < min_freq && freq_next_[blk] != NULL_PTR) {
            blk = freq_next_[blk];
        }

        freq_value_[ctx] = freq_value_[blk];
        freq_char_[ctx] = freq_char_[blk];

        u16 next = freq_next_[blk];
        freq_next_[blk] = free_block_head_;
        free_block_head_ = freq_next_[ctx];
        freq_next_[ctx] = next;

        if (next == NULL_PTR) {
            ctx_char_count_[ctx] = 0;
            ctx_total_freq_[ctx] = freq_value_[ctx];
            ctx_low_freq_count_[ctx] = (ctx_total_freq_[ctx] < LOW_FREQ_THRESHOLD) ? 1 : 0;
            return;
        }
    }

    freq_value_[ctx] /= min_freq;
    ctx_total_freq_[ctx] = freq_value_[ctx];
    ctx_low_freq_count_[ctx] = (ctx_total_freq_[ctx] < LOW_FREQ_THRESHOLD) ? 1 : 0;
    ctx_char_count_[ctx] = 0;

    size_t prev = ctx;
    blk = freq_next_[prev];

    while (blk != NULL_PTR) {
        if (freq_value_[blk] < min_freq) {
            freq_next_[prev] = freq_next_[blk];
            freq_next_[blk] = free_block_head_;
            free_block_head_ = blk;
            blk = freq_next_[prev];
        } else {
            ctx_char_count_[ctx]++;
            freq_value_[blk] /= min_freq;
            ctx_total_freq_[ctx] += freq_value_[blk];
            if (freq_value_[blk] < LOW_FREQ_THRESHOLD) ctx_low_freq_count_[ctx]++;
            prev = blk;
            blk = freq_next_[prev];
        }
    }
}

u16 ha_hsc_decompressor::allocate_context(u8 order, u8 first_char) {
    u16 new_ctx = lru_back_;
    lru_back_ = lru_prev_[new_ctx];

    lru_prev_[lru_front_] = new_ctx;
    lru_next_[new_ctx] = lru_front_;
    lru_front_ = new_ctx;

    size_t idx = new_ctx;

    if (ctx_length_[idx] != 0xFF) {
        if (ctx_length_[idx] == MAX_ORDER) {
            order_reduction_counter_--;
            if (order_reduction_counter_ == 0) {
                current_max_order_ = static_cast<u8>(MAX_ORDER - 1);
            }
        }

        u16 hash = compute_hash(ctx_bytes_[idx], ctx_length_[idx]);

        if (hash_heads_[hash] == new_ctx) {
            hash_heads_[hash] = hash_chain_[idx];
        } else {
            u16 prev = hash_heads_[hash];
            while (hash_chain_[prev] != new_ctx) {
                prev = hash_chain_[prev];
            }
            hash_chain_[prev] = hash_chain_[idx];
        }

        if (freq_next_[idx] != NULL_PTR) {
            u16 last = freq_next_[idx];
            while (freq_next_[last] != NULL_PTR) {
                last = freq_next_[last];
            }
            freq_next_[last] = free_block_head_;
            free_block_head_ = freq_next_[idx];
        }
    }

    freq_next_[idx] = NULL_PTR;
    ctx_low_freq_count_[idx] = 1;
    ctx_total_freq_[idx] = 1;
    freq_value_[idx] = 1;
    freq_char_[idx] = first_char;
    ctx_rescale_factor_[idx] = RESCALE_FACTOR_INIT;
    ctx_char_count_[idx] = 0;
    ctx_length_[idx] = order;
    ctx_bytes_[idx] = context_window_;

    u16 hash = compute_hash(context_window_, order);
    hash_chain_[idx] = hash_heads_[hash];
    hash_heads_[hash] = new_ctx;

    return new_ctx;
}

void ha_hsc_decompressor::update_model(u8 character) {
    while (update_depth_ != 0) {
        update_depth_--;

        u16 block = update_blocks_[update_depth_];
        u16 ctx_id = update_contexts_[update_depth_];

        if (ctx_id & 0x8000) {
            ctx_id &= 0x7FFF;
            size_t idx = ctx_id;

            if (free_block_head_ == NULL_PTR) {
                reclaim_blocks();
            }

            u16 new_block = free_block_head_;
            freq_next_[block] = new_block;
            free_block_head_ = freq_next_[new_block];
            freq_next_[new_block] = NULL_PTR;
            freq_value_[new_block] = 1;
            freq_char_[new_block] = character;
            ctx_char_count_[idx]++;
            ctx_low_freq_count_[idx]++;

            update_context_stats(idx, new_block);
        } else {
            size_t idx = ctx_id;
            freq_value_[block]++;

            if (freq_value_[block] == LOW_FREQ_THRESHOLD) {
                if (ctx_low_freq_count_[idx] > 0) ctx_low_freq_count_[idx]--;
            }

            update_context_stats(idx, block);
        }
    }
}

void ha_hsc_decompressor::update_context_stats(size_t idx, u16 block) {
    ctx_total_freq_[idx]++;

    u16 char_divisor = static_cast<u16>(ctx_char_count_[idx]) + 1;
    if ((freq_value_[block] << 1) < ctx_total_freq_[idx] / char_divisor) {
        if (ctx_rescale_factor_[idx] > 0) ctx_rescale_factor_[idx]--;
    } else if (ctx_rescale_factor_[idx] < RESCALE_FACTOR_INIT) {
        ctx_rescale_factor_[idx]++;
    }

    if (ctx_rescale_factor_[idx] == 0 || ctx_total_freq_[idx] >= MAX_TOTAL_FREQ) {
        ctx_rescale_factor_[idx]++;
        ctx_low_freq_count_[idx] = 0;
        ctx_total_freq_[idx] = 0;

        u16 blk = static_cast<u16>(idx);
        while (blk != NULL_PTR) {
            if (freq_value_[blk] > 1) {
                freq_value_[blk] >>= 1;
                ctx_total_freq_[idx] += freq_value_[blk];
                if (freq_value_[blk] < LOW_FREQ_THRESHOLD) ctx_low_freq_count_[idx]++;
            } else {
                ctx_total_freq_[idx]++;
                ctx_low_freq_count_[idx]++;
            }
            blk = freq_next_[blk];
        }
    }
}

result_t<stream_result> ha_hsc_decompressor::decompress_some(
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

    while (state_ != state::DONE) {
        switch (state_) {
            case state::FIND_CONTEXT: {
                current_ctx_id_ = find_longest_context();
                min_order_ = (current_ctx_id_ == NULL_PTR) ? 0 : ctx_length_[current_ctx_id_] + 1;
                max_order_ = current_max_order_ + 1;
                phase_ = decode_phase::THRESHOLD;
                state_ = (current_ctx_id_ == NULL_PTR) ? state::DECODE_UNIFORM : state::DECODE_FROM_CONTEXT;
                break;
            }

            case state::DECODE_FROM_CONTEXT: {
                // Decode from context (with or without exclusions)
                size_t idx = current_ctx_id_;
                bool has_exclusions = !excluded_stack_.empty();

                if (phase_ == decode_phase::THRESHOLD) {
                    // Calculate total and escape
                    if (has_exclusions) {
                        total_ = 0;
                        low_count_ = 0;
                        u16 blk = current_ctx_id_;
                        while (blk != NULL_PTR) {
                            u8 ch = freq_char_[blk];
                            if (!excluded_[ch]) {
                                u16 freq = freq_value_[blk];
                                total_ += freq;
                                if (freq < LOW_FREQ_THRESHOLD) low_count_++;
                            }
                            blk = freq_next_[blk];
                        }
                        escape_ = calculate_escape_probability(low_count_, current_ctx_id_);
                    } else {
                        escape_ = calculate_escape_probability(ctx_low_freq_count_[idx], current_ctx_id_);
                        total_ = ctx_total_freq_[idx];

                        scale_ = 0;
                        if (non_escape_count_ >= NON_ESCAPE_THRESHOLD) {
                            scale_ = (total_ <= NON_ESCAPE_TOTAL_LIMIT && non_escape_count_ == NON_ESCAPE_MAX) ? 2 : 1;
                        }
                    }

                    u16 scaled_total = (!has_exclusions && scale_ > 0) ? (total_ << scale_) : total_;
                    if (!coder_.try_threshold_val(in_ptr, in_end, scaled_total + escape_, threshold_, input_finished)) {
                        if (input_finished) {
                            state_ = state::DONE;
                            return stream_result::done(bytes_read(), bytes_written());
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    if (!has_exclusions && scale_ > 0) {
                        threshold_ >>= scale_;
                    }
                    phase_ = decode_phase::NAVIGATE;
                }

                if (phase_ == decode_phase::NAVIGATE) {
                    // Navigate to find symbol
                    block_ = current_ctx_id_;
                    cumulative_ = 0;
                    symbol_freq_ = 0;

                    while (block_ != NULL_PTR) {
                        u8 ch = freq_char_[block_];
                        if (has_exclusions && excluded_[ch]) {
                            block_ = freq_next_[block_];
                            continue;
                        }

                        u16 freq = freq_value_[block_];
                        if (cumulative_ + freq > threshold_) {
                            symbol_freq_ = freq;
                            if (!has_exclusions && scale_ > 0) {
                                symbol_freq_ <<= scale_;
                            }
                            break;
                        }
                        cumulative_ += freq;
                        block_ = freq_next_[block_];
                    }

                    if (!has_exclusions && scale_ > 0) {
                        cumulative_ <<= scale_;
                    }
                    phase_ = decode_phase::UPDATE;
                }

                if (phase_ == decode_phase::UPDATE) {
                    u16 scaled_total = (scale_ > 0 && !has_exclusions) ? (total_ << scale_) : total_;

                    if (block_ != NULL_PTR) {
                        // Found symbol
                        if (!coder_.try_decode_update(in_ptr, in_end, cumulative_, cumulative_ + symbol_freq_, scaled_total + escape_, input_finished)) {
                            if (input_finished) {
                                return std::unexpected(error{error_code::InputBufferUnderflow});
                            }
                            return stream_result::need_input(bytes_read(), bytes_written());
                        }

                        if (ctx_total_freq_[idx] == 1 && initial_escape_[ctx_length_[idx]] > 0) {
                            initial_escape_[ctx_length_[idx]]--;
                        }

                        if (!has_exclusions) {
                            update_depth_ = 1;
                            update_blocks_[0] = block_;
                            update_contexts_[0] = current_ctx_id_;
                            if (non_escape_count_ < NON_ESCAPE_MAX) non_escape_count_++;
                        } else {
                            update_blocks_[update_depth_] = block_;
                            update_contexts_[update_depth_] = current_ctx_id_;
                            update_depth_++;
                            non_escape_count_++;
                        }

                        promote_to_front(current_ctx_id_);
                        decoded_symbol_ = static_cast<u16>(freq_char_[block_]);
                        phase_ = decode_phase::THRESHOLD;
                        state_ = (decoded_symbol_ == ESCAPE_SYMBOL) ? state::DONE : state::UPDATE_MODEL;
                    } else {
                        // Escape
                        if (!coder_.try_decode_update(in_ptr, in_end, scaled_total, scaled_total + escape_, scaled_total + escape_, input_finished)) {
                            if (input_finished) {
                                return std::unexpected(error{error_code::InputBufferUnderflow});
                            }
                            return stream_result::need_input(bytes_read(), bytes_written());
                        }

                        if (ctx_total_freq_[idx] == 1 && initial_escape_[ctx_length_[idx]] < ESCAPE_COUNTER_LIMIT) {
                            initial_escape_[ctx_length_[idx]]++;
                        }

                        // Add excluded chars
                        u16 blk = current_ctx_id_;
                        u16 last = 0;
                        while (blk != NULL_PTR) {
                            u8 ch = freq_char_[blk];
                            if (!excluded_[ch]) {
                                excluded_stack_.push_back(ch);
                                excluded_[ch] = true;
                            }
                            last = blk;
                            blk = freq_next_[blk];
                        }

                        if (!has_exclusions) {
                            update_depth_ = 1;
                            update_contexts_[0] = 0x8000 | current_ctx_id_;
                            update_blocks_[0] = last;
                            non_escape_count_ = 0;
                        } else {
                            update_contexts_[update_depth_] = 0x8000 | current_ctx_id_;
                            update_blocks_[update_depth_] = last;
                            update_depth_++;
                        }

                        // Find next context
                        current_ctx_id_ = find_next_context();
                        phase_ = decode_phase::THRESHOLD;
                        state_ = (current_ctx_id_ == NULL_PTR) ? state::DECODE_UNIFORM : state::DECODE_FROM_CONTEXT;
                    }
                }
                break;
            }

            case state::DECODE_UNIFORM: {
                // Decode uniformly (no context found)
                u16 unmasked_count = 257 - static_cast<u16>(excluded_stack_.size());

                if (phase_ == decode_phase::THRESHOLD) {
                    if (!coder_.try_threshold_val(in_ptr, in_end, unmasked_count, threshold_, input_finished)) {
                        if (input_finished) {
                            state_ = state::DONE;
                            return stream_result::done(bytes_read(), bytes_written());
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    symbol_ = 0;
                    cumulative_ = 0;
                    phase_ = decode_phase::UNIFORM_SCAN;
                }

                if (phase_ == decode_phase::UNIFORM_SCAN) {
                    while (symbol_ < 256) {
                        if (excluded_[symbol_]) {
                            symbol_++;
                            continue;
                        }
                        if (cumulative_ + 1 > threshold_) break;
                        cumulative_++;
                        symbol_++;
                    }
                    phase_ = decode_phase::UPDATE;
                }

                if (phase_ == decode_phase::UPDATE) {
                    if (!coder_.try_decode_update(in_ptr, in_end, cumulative_, cumulative_ + 1, unmasked_count, input_finished)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    decoded_symbol_ = symbol_;
                    phase_ = decode_phase::THRESHOLD;
                    state_ = (decoded_symbol_ == ESCAPE_SYMBOL) ? state::DONE : state::UPDATE_MODEL;
                }
                break;
            }

            case state::UPDATE_MODEL: {
                decoded_char_ = static_cast<u8>(decoded_symbol_);
                update_model(decoded_char_);
                state_ = state::ALLOCATE_CONTEXTS;
                break;
            }

            case state::ALLOCATE_CONTEXTS: {
                while (max_order_ > min_order_) {
                    max_order_--;
                    allocate_context(max_order_, decoded_char_);
                }
                state_ = state::OUTPUT_CHAR;
                break;
            }

            case state::OUTPUT_CHAR: {
                if (out_ptr >= out_end) {
                    return stream_result::need_output(bytes_read(), bytes_written());
                }
                *out_ptr++ = decoded_char_;
                advance_context(decoded_char_);
                phase_ = decode_phase::THRESHOLD;
                state_ = state::FIND_CONTEXT;
                break;
            }

            case state::DONE:
                break;
        }
    }

    return stream_result::done(bytes_read(), bytes_written());
}

}  // namespace crate
