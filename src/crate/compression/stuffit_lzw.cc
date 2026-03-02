#include <crate/compression/stuffit_lzw.hh>
#include <cstring>

namespace crate {

stuffit_lzw_decompressor::stuffit_lzw_decompressor() {
    reset();
}

void stuffit_lzw_decompressor::reset() {
    state_ = state::READ_FIRST_CODE;

    // Initialize suffix table with identity for first 256 entries
    for (int i = 0; i < 256; i++) {
        tab_suffix_[static_cast<size_t>(i)] = static_cast<u8>(i);
        tab_prefix_[static_cast<size_t>(i)] = 0;
    }

    stack_ptr_ = 0;
    stack_out_ptr_ = 0;

    n_bits_ = INIT_BITS;
    maxcode_ = (1 << n_bits_) - 1;
    max_maxcode_ = 1 << MAX_BITS;
    free_ent_ = FIRST_CODE;
    clear_flg_ = false;

    oldcode_ = -1;
    finchar_ = 0;
    incode_ = 0;

    gbuf_.fill(0);
    bit_offset_ = 0;
    bits_in_buffer_ = 0;
    gbuf_bytes_ = 0;
}

void stuffit_lzw_decompressor::clear_table() {
    for (int i = 0; i < 256; i++) {
        tab_prefix_[static_cast<size_t>(i)] = 0;
    }
    clear_flg_ = true;
    free_ent_ = FIRST_CODE;
    oldcode_ = -1;
}

int stuffit_lzw_decompressor::get_code(const byte*& ptr, const byte* end) {
    // Handle code size increase after clear or when table fills
    if (clear_flg_ || free_ent_ > maxcode_) {
        if (free_ent_ > maxcode_) {
            n_bits_++;
            if (n_bits_ == MAX_BITS) {
                maxcode_ = max_maxcode_;
            } else {
                maxcode_ = (1 << n_bits_) - 1;
            }
        }
        if (clear_flg_) {
            n_bits_ = INIT_BITS;
            maxcode_ = (1 << n_bits_) - 1;
            clear_flg_ = false;
        }
        // Reset buffer when bit size changes
        bit_offset_ = 0;
        bits_in_buffer_ = 0;
        gbuf_bytes_ = 0;
    }

    // If we've exhausted the current group, read a new one
    if (bit_offset_ >= bits_in_buffer_) {
        // Read n_bits_ bytes into buffer
        size_t bytes_to_read = static_cast<size_t>(n_bits_);
        size_t available = static_cast<size_t>(end - ptr);
        if (available < bytes_to_read) {
            // Not enough input - try to read what we can
            if (available == 0) {
                return -1;  // Need more input
            }
            bytes_to_read = available;
        }

        std::memcpy(gbuf_.data(), ptr, bytes_to_read);
        ptr += bytes_to_read;
        gbuf_bytes_ = bytes_to_read;
        bit_offset_ = 0;
        // Round down to integral number of codes
        bits_in_buffer_ = static_cast<int>((bytes_to_read << 3) - (static_cast<size_t>(n_bits_) - 1));

        if (bits_in_buffer_ <= 0) {
            return -1;  // Need more input
        }
    }

    // Extract code from buffer (LSB first bit packing)
    int r_off = bit_offset_;
    const u8* bp = gbuf_.data() + (r_off >> 3);
    r_off &= 7;

    // Get first part (low order bits)
    int gcode = (*bp++ >> r_off);
    int bits = n_bits_ - (8 - r_off);

    // Get middle 8-bit parts
    if (bits >= 8) {
        gcode |= (*bp++ << (8 - r_off));
        bits -= 8;
    }

    // Get high order bits
    if (bits > 0) {
        gcode |= (*bp & ((1 << bits) - 1)) << (n_bits_ - bits);
    }

    bit_offset_ += n_bits_;
    return gcode & ((1 << n_bits_) - 1);
}

result_t<stream_result> stuffit_lzw_decompressor::decompress_some(
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
        // First, output any pending stack data
        if (stack_out_ptr_ < stack_ptr_) {
            while (stack_out_ptr_ < stack_ptr_) {
                if (out_ptr >= out_end) {
                    return stream_result::need_output(bytes_read(), bytes_written());
                }
                // Output in reverse order (stack was built backwards)
                *out_ptr++ = stack_[stack_ptr_ - 1 - stack_out_ptr_];
                stack_out_ptr_++;
            }
            // Stack fully output, reset
            stack_ptr_ = 0;
            stack_out_ptr_ = 0;
        }

        switch (state_) {
            case state::READ_FIRST_CODE: {
                int code = get_code(in_ptr, in_end);
                if (code < 0) {
                    if (input_finished) {
                        state_ = state::DONE;
                        return stream_result::done(bytes_read(), bytes_written());
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                oldcode_ = code;
                finchar_ = code;

                if (out_ptr >= out_end) {
                    // Need to output first byte
                    stack_[stack_ptr_++] = static_cast<u8>(finchar_);
                    state_ = state::OUTPUT_STRING;
                    break;
                }

                *out_ptr++ = static_cast<byte>(finchar_);
                state_ = state::READ_CODE;
                break;
            }

            case state::READ_CODE: {
                int code = get_code(in_ptr, in_end);
                if (code < 0) {
                    if (input_finished) {
                        state_ = state::DONE;
                        return stream_result::done(bytes_read(), bytes_written());
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                // Handle CLEAR code
                if (code == CLEAR_CODE) {
                    clear_table();
                    continue;  // Read next code
                }

                incode_ = code;

                // Special case for KwKwK string
                if (code >= free_ent_) {
                    if (code > free_ent_ || oldcode_ < 0) {
                        return crate::make_unexpected(error{error_code::CorruptData, "Bad LZW code"});
                    }
                    stack_[stack_ptr_++] = static_cast<u8>(finchar_);
                    code = oldcode_;
                }

                // Walk the chain to build output (in reverse)
                while (code >= 256) {
                    if (stack_ptr_ >= STACK_SIZE) {
                        return crate::make_unexpected(error{error_code::CorruptData, "LZW stack overflow"});
                    }
                    stack_[stack_ptr_++] = tab_suffix_[static_cast<size_t>(code)];
                    code = tab_prefix_[static_cast<size_t>(code)];
                }
                finchar_ = tab_suffix_[static_cast<size_t>(code)];
                stack_[stack_ptr_++] = static_cast<u8>(finchar_);

                // Add new entry to table
                if (free_ent_ < max_maxcode_ && oldcode_ >= 0) {
                    tab_prefix_[static_cast<size_t>(free_ent_)] = static_cast<u16>(oldcode_);
                    tab_suffix_[static_cast<size_t>(free_ent_)] = static_cast<u8>(finchar_);
                    free_ent_++;
                }

                oldcode_ = incode_;

                // Output the string (stack_ptr_ > 0 now)
                state_ = state::OUTPUT_STRING;
                break;
            }

            case state::OUTPUT_STRING: {
                // Output pending stack data
                while (stack_out_ptr_ < stack_ptr_) {
                    if (out_ptr >= out_end) {
                        return stream_result::need_output(bytes_read(), bytes_written());
                    }
                    *out_ptr++ = stack_[stack_ptr_ - 1 - stack_out_ptr_];
                    stack_out_ptr_++;
                }
                stack_ptr_ = 0;
                stack_out_ptr_ = 0;
                state_ = state::READ_CODE;
                break;
            }

            case state::DONE:
                break;
        }
    }

    return stream_result::done(bytes_read(), bytes_written());
}

}  // namespace crate
