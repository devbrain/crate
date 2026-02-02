#include <crate/compression/arj_lz.hh>

namespace crate {

arj_method4_decompressor::arj_method4_decompressor(bool old_format) : old_format_(old_format) {
    init_state();
}

void arj_method4_decompressor::reset() {
    init_state();
}

void arj_method4_decompressor::init_state() {
    window_pos_ = 0;
    std::fill(window_.begin(), window_.end(), 0);

    bit_buffer_ = 0;
    bits_left_ = 0;

    state_ = state::READ_LENGTH_PREFIX;
    length_ones_count_ = 0;
    length_value_ = 0;
    match_length_ = 0;
    match_offset_ = 0;
    match_remaining_ = 0;
    offset_ones_count_ = 0;
}

bool arj_method4_decompressor::try_peek_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
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

bool arj_method4_decompressor::try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
    if (!try_peek_bits(ptr, end, n, out)) {
        return false;
    }
    remove_bits(n);
    return true;
}

void arj_method4_decompressor::remove_bits(unsigned n) {
    bits_left_ -= n;
}

result_t<stream_result> arj_method4_decompressor::decompress_some(
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
            case state::READ_LENGTH_PREFIX: {
                // Read unary prefix: count consecutive 1s until we see a 0 (max 7)
                while (length_ones_count_ < 7) {
                    u32 bit = 0;
                    if (!try_read_bits(in_ptr, in_end, 1, bit)) {
                        if (input_finished) {
                            // End of stream - graceful exit if we have output
                            if (bytes_written() > 0) {
                                state_ = state::DONE;
                                return stream_result::done(bytes_read(), bytes_written());
                            }
                            state_ = state::DONE;
                            return stream_result::done(bytes_read(), bytes_written());
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    if (bit == 0) {
                        break;  // End of unary prefix
                    }
                    length_ones_count_++;
                }

                // Old format has an extra bit after 7 ones
                if (length_ones_count_ >= 7 && old_format_) {
                    u32 extra = 0;
                    if (!try_read_bits(in_ptr, in_end, 1, extra)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    // Extra bit is consumed but not used
                }

                if (length_ones_count_ == 0) {
                    // Literal byte - read 8 bits
                    state_ = state::READ_LITERAL;
                } else {
                    // Match - need to read extra bits for length value
                    state_ = state::READ_LENGTH_EXTRA;
                }
                break;
            }

            case state::READ_LENGTH_EXTRA: {
                // Read 'length_ones_count_' more bits for the length value
                u32 value_bits = 0;
                if (!try_read_bits(in_ptr, in_end, length_ones_count_, value_bits)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                // length_value = (1 << ones_count) - 1 + value_bits
                // This gives the length code, actual length = length_code + 2
                length_value_ = ((1u << length_ones_count_) - 1) + value_bits;
                match_length_ = length_value_ + 2;

                // Reset for next code
                length_ones_count_ = 0;

                // Now read offset
                offset_ones_count_ = 0;
                state_ = state::READ_OFFSET_PREFIX;
                break;
            }

            case state::READ_LITERAL: {
                u32 value = 0;
                if (!try_read_bits(in_ptr, in_end, 8, value)) {
                    if (input_finished) {
                        // End of stream - if we have output, that's success
                        if (bytes_written() > 0) {
                            state_ = state::DONE;
                            return stream_result::done(bytes_read(), bytes_written());
                        }
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                // Check output space
                if (out_ptr >= out_end) {
                    // We read the literal but can't write it - need to save state
                    // Actually, let's check output space before reading
                    // For now, we've already read, so error out
                    return stream_result::need_output(bytes_read(), bytes_written());
                }

                u8 b = static_cast<u8>(value);
                *out_ptr++ = b;
                window_[window_pos_++ & WINDOW_MASK] = b;

                // Reset for next code
                length_ones_count_ = 0;
                state_ = state::READ_LENGTH_PREFIX;

                // Check if output buffer is full
                if (out_ptr >= out_end) {
                    return stream_result::need_output(bytes_read(), bytes_written());
                }
                break;
            }

            case state::READ_OFFSET_PREFIX: {
                // Read unary prefix for offset: count consecutive 1s until 0 (max 4)
                while (offset_ones_count_ < 4) {
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
                    offset_ones_count_++;
                }

                // Old format has an extra bit after 4 ones
                if (offset_ones_count_ >= 4 && old_format_) {
                    u32 extra = 0;
                    if (!try_read_bits(in_ptr, in_end, 1, extra)) {
                        if (input_finished) {
                            return std::unexpected(error{error_code::InputBufferUnderflow});
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                }

                state_ = state::READ_OFFSET_EXTRA;
                break;
            }

            case state::READ_OFFSET_EXTRA: {
                // Read (9 + offset_ones_count_) bits for offset value
                unsigned extra_bits = 9 + offset_ones_count_;
                u32 value_bits = 0;
                if (!try_read_bits(in_ptr, in_end, extra_bits, value_bits)) {
                    if (input_finished) {
                        return std::unexpected(error{error_code::InputBufferUnderflow});
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                // offset = (1 << (9 + ones_count)) - 512 + value_bits
                match_offset_ = ((1u << extra_bits) - 512) + value_bits;
                match_remaining_ = match_length_;

                state_ = state::COPY_MATCH;
                break;
            }

            case state::COPY_MATCH: {
                while (match_remaining_ > 0) {
                    if (out_ptr >= out_end) {
                        return stream_result::need_output(bytes_read(), bytes_written());
                    }

                    u8 value = window_[(window_pos_ - match_offset_ - 1) & WINDOW_MASK];
                    *out_ptr++ = value;
                    window_[window_pos_++ & WINDOW_MASK] = value;
                    match_remaining_--;
                }

                // Reset for next code
                length_ones_count_ = 0;
                state_ = state::READ_LENGTH_PREFIX;

                // Check if output buffer is full
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
