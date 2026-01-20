#include <crate/compression/lzss.hh>

namespace crate {

szdd_lzss_decompressor::szdd_lzss_decompressor() {
    init_state();
}

result_t<stream_result> szdd_lzss_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    size_t in_pos = 0;
    size_t out_pos = 0;

    // Helper to advance to next bit, returning to READ_CONTROL after bit 7
    auto advance_bit = [this]() {
        current_bit_++;
        if (current_bit_ >= 8) {
            current_bit_ = 0;
            state_ = state::READ_CONTROL;
        }
    };

    // Main state machine loop
    while (state_ != state::DONE) {
        switch (state_) {
            case state::READ_CONTROL: {
                if (in_pos >= input.size()) {
                    // Need more input
                    if (input_finished) {
                        // No more input coming - we're done
                        state_ = state::DONE;
                    }
                    goto exit_loop;
                }
                control_byte_ = input[in_pos++];
                current_bit_ = 0;
                started_ = true;

                // Determine next state based on first bit
                if (control_byte_ & (1 << current_bit_)) {
                    state_ = state::READ_LITERAL;
                } else {
                    state_ = state::READ_MATCH_LO;
                }
                break;
            }

            case state::READ_LITERAL: {
                if (in_pos >= input.size()) {
                    if (input_finished) {
                        // Truncated - expected literal byte
                        state_ = state::DONE;
                    }
                    goto exit_loop;
                }
                if (out_pos >= output.size()) {
                    // Output buffer full
                    goto exit_loop;
                }

                u8 value = input[in_pos++];
                output[out_pos++] = value;
                window_[window_pos_++ & WINDOW_MASK] = value;

                // Move to next bit
                advance_bit();
                if (state_ == state::READ_CONTROL) {
                    // Will read new control byte on next iteration
                } else {
                    // Determine state for next bit
                    if (control_byte_ & (1 << current_bit_)) {
                        state_ = state::READ_LITERAL;
                    } else {
                        state_ = state::READ_MATCH_LO;
                    }
                }
                break;
            }

            case state::READ_MATCH_LO: {
                if (in_pos >= input.size()) {
                    if (input_finished) {
                        state_ = state::DONE;
                    }
                    goto exit_loop;
                }
                match_lo_ = input[in_pos++];
                state_ = state::READ_MATCH_HI;
                break;
            }

            case state::READ_MATCH_HI: {
                if (in_pos >= input.size()) {
                    if (input_finished) {
                        state_ = state::DONE;
                    }
                    goto exit_loop;
                }
                u8 hi = input[in_pos++];
                match_pos_ = match_lo_ | ((hi & 0xF0) << 4);
                match_remaining_ = (hi & 0x0F) + MIN_MATCH;
                state_ = state::COPY_MATCH;
                break;
            }

            case state::COPY_MATCH: {
                // Copy as many bytes as we can
                while (match_remaining_ > 0 && out_pos < output.size()) {
                    u8 value = window_[match_pos_++ & WINDOW_MASK];
                    output[out_pos++] = value;
                    window_[window_pos_++ & WINDOW_MASK] = value;
                    match_remaining_--;
                }

                if (match_remaining_ > 0) {
                    // Output buffer full, need to continue later
                    goto exit_loop;
                }

                // Match complete, move to next bit
                advance_bit();
                if (state_ == state::READ_CONTROL) {
                    // Will read new control byte
                } else {
                    if (control_byte_ & (1 << current_bit_)) {
                        state_ = state::READ_LITERAL;
                    } else {
                        state_ = state::READ_MATCH_LO;
                    }
                }
                break;
            }

            case state::DONE:
                goto exit_loop;
        }
    }

exit_loop:
    // If input_finished and we consumed all input, we're done
    if (input_finished && in_pos >= input.size() && state_ != state::COPY_MATCH) {
        state_ = state::DONE;
    }

    if (state_ == state::DONE) {
        return stream_result::done(in_pos, out_pos);
    }

    // Determine why we stopped: output full or need more input
    // If we're in COPY_MATCH and stopped, output must be full
    // If READ_LITERAL stopped with full output buffer, output is full
    if (out_pos >= output.size()) {
        return stream_result::need_output(in_pos, out_pos);
    }
    return stream_result::need_input(in_pos, out_pos);
}

void szdd_lzss_decompressor::reset() {
    init_state();
}

void szdd_lzss_decompressor::init_state() {
    window_pos_ = INITIAL_POS;
    std::fill(window_.begin(), window_.end(), ' ');
    state_ = state::READ_CONTROL;
    control_byte_ = 0;
    current_bit_ = 0;
    match_lo_ = 0;
    match_pos_ = 0;
    match_remaining_ = 0;
    started_ = false;
}

bool kwaj_lzss_decompressor::try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
    while (bits_left_ < n) {
        if (ptr >= end) {
            return false;
        }
        bit_buffer_ |= static_cast <u64>(*ptr++) << bits_left_;
        bits_left_ += 8;
    }

    out = static_cast <u32>(bit_buffer_ & ((1ULL << n) - 1));
    bit_buffer_ >>= n;
    bits_left_ -= n;
    return true;
}

result_t<stream_result> kwaj_lzss_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    const byte* in_ptr = input.data();
    const byte* in_end = input.data() + input.size();
    byte* out_ptr = output.data();
    byte* out_end = output.data() + output.size();

    while (state_ != state::DONE) {
        switch (state_) {
            case state::READ_FLAG: {
                u32 flag = 0;
                if (!try_read_bits(in_ptr, in_end, 1, flag)) {
                    if (input_finished) {
                        state_ = state::DONE;
                        return stream_result::done(
                            static_cast <size_t>(in_ptr - input.data()),
                            static_cast <size_t>(out_ptr - output.data())
                        );
                    }
                    return stream_result::need_input(
                        static_cast <size_t>(in_ptr - input.data()),
                        static_cast <size_t>(out_ptr - output.data())
                    );
                }
                state_ = flag ? state::READ_LITERAL : state::READ_MATCH_LEN;
                break;
            }

            case state::READ_LITERAL: {
                u32 value = 0;
                if (!try_read_bits(in_ptr, in_end, 8, value)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(
                        static_cast <size_t>(in_ptr - input.data()),
                        static_cast <size_t>(out_ptr - output.data())
                    );
                }
                pending_literal_ = static_cast <u8>(value);
                state_ = state::WRITE_LITERAL;
                break;
            }

            case state::WRITE_LITERAL:
                if (out_ptr >= out_end) {
                    return stream_result::need_output(
                        static_cast <size_t>(in_ptr - input.data()),
                        static_cast <size_t>(out_ptr - output.data())
                    );
                }
                *out_ptr++ = pending_literal_;
                window_[window_pos_++ & WINDOW_MASK] = pending_literal_;
                state_ = state::READ_FLAG;
                break;

            case state::READ_MATCH_LEN: {
                u32 len_bits = 0;
                if (!try_read_bits(in_ptr, in_end, 4, len_bits)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(
                        static_cast <size_t>(in_ptr - input.data()),
                        static_cast <size_t>(out_ptr - output.data())
                    );
                }
                match_length_ = static_cast <u8>(len_bits + 3);
                state_ = state::READ_MATCH_OFF;
                break;
            }

            case state::READ_MATCH_OFF: {
                u32 off_bits = 0;
                if (!try_read_bits(in_ptr, in_end, 12, off_bits)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(
                        static_cast <size_t>(in_ptr - input.data()),
                        static_cast <size_t>(out_ptr - output.data())
                    );
                }
                match_offset_ = static_cast <u16>(off_bits);
                match_remaining_ = match_length_;
                match_pos_ = (window_pos_ - match_offset_ - 1) & WINDOW_MASK;
                state_ = state::COPY_MATCH;
                break;
            }

            case state::COPY_MATCH:
                while (match_remaining_ > 0) {
                    if (out_ptr >= out_end) {
                        return stream_result::need_output(
                            static_cast <size_t>(in_ptr - input.data()),
                            static_cast <size_t>(out_ptr - output.data())
                        );
                    }
                    u8 value = window_[match_pos_ & WINDOW_MASK];
                    match_pos_++;
                    *out_ptr++ = value;
                    window_[window_pos_++ & WINDOW_MASK] = value;
                    match_remaining_--;
                }
                state_ = state::READ_FLAG;
                break;

            case state::DONE:
                break;
        }
    }

    return stream_result::done(
        static_cast <size_t>(in_ptr - input.data()),
        static_cast <size_t>(out_ptr - output.data())
    );
}

}  // namespace crate
