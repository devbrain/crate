#include <crate/compression/zoo_lzw.hh>

namespace crate {

void zoo_lzw_decompressor::init_state() {
    state_ = state::READ_FIRST_CODE;
    bit_buffer_ = 0;
    bits_left_ = 0;
    init_dictionary();
    code_ = 0;
    oldcode_ = 0;
    incode_ = 0;
    finchar_ = 0;
    stack_.clear();
    stack_.reserve(4096);
    stack_pos_ = 0;
}

void zoo_lzw_decompressor::init_dictionary() {
    n_bits_ = INIT_BITS;
    max_code_ = (1 << n_bits_) - 1;
    free_ent_ = FIRST;

    // Initialize suffix table with identity for literal codes
    for (int i = 0; i < 256; i++) {
        suffix_[static_cast<size_t>(i)] = static_cast<u8>(i);
    }
}

bool zoo_lzw_decompressor::try_read_code(const byte*& ptr, const byte* end, int& code) {
    // Read n_bits_ bits, LSB first
    while (bits_left_ < static_cast<unsigned>(n_bits_)) {
        if (ptr >= end) {
            return false;
        }
        bit_buffer_ |= static_cast<u32>(*ptr++) << bits_left_;
        bits_left_ += 8;
    }

    code = static_cast<int>(bit_buffer_ & ((1u << n_bits_) - 1));
    bit_buffer_ >>= n_bits_;
    bits_left_ -= static_cast<unsigned>(n_bits_);
    return true;
}

result_t<stream_result> zoo_lzw_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    if (!expected_output_set()) {
        return crate::make_unexpected(error{
            error_code::InvalidParameter,
            "Expected size required for bounded decompression"
        });
    }

    const byte* in_ptr = input.data();
    const byte* in_end = input.data() + input.size();
    byte* out_ptr = output.data();
    byte* out_end = output.data() + output.size();

    // Limit output to expected size
    size_t remaining = expected_output_size() - total_output_written();
    if (static_cast<size_t>(out_end - out_ptr) > remaining) {
        out_end = out_ptr + remaining;
    }

    auto bytes_read = [&]() -> size_t {
        return static_cast<size_t>(in_ptr - input.data());
    };
    auto bytes_written = [&]() -> size_t {
        return static_cast<size_t>(out_ptr - output.data());
    };
    auto finalize = [&](decode_status status) -> stream_result {
        size_t written = bytes_written();
        advance_output(written);

        if (state_ == state::DONE || total_output_written() >= expected_output_size()) {
            state_ = state::DONE;
            return stream_result::done(bytes_read(), written);
        }

        if (status == decode_status::needs_more_input) {
            return stream_result::need_input(bytes_read(), written);
        }
        if (status == decode_status::needs_more_output) {
            return stream_result::need_output(bytes_read(), written);
        }

        return stream_result::done(bytes_read(), written);
    };

    while (state_ != state::DONE) {
        // Check if we've written enough
        if (total_output_written() + bytes_written() >= expected_output_size()) {
            state_ = state::DONE;
            break;
        }

        switch (state_) {
            case state::READ_FIRST_CODE: {
                if (!try_read_code(in_ptr, in_end, code_)) {
                    if (input_finished) {
                        state_ = state::DONE;
                        return finalize(decode_status::done);
                    }
                    return finalize(decode_status::needs_more_input);
                }

                if (code_ == CLEAR) {
                    state_ = state::HANDLE_INITIAL_CLEAR;
                } else {
                    finchar_ = static_cast<u8>(code_);
                    oldcode_ = code_;
                    state_ = state::OUTPUT_FIRST_CHAR;
                }
                break;
            }

            case state::HANDLE_INITIAL_CLEAR: {
                if (!try_read_code(in_ptr, in_end, code_)) {
                    if (input_finished) {
                        state_ = state::DONE;
                        return finalize(decode_status::done);
                    }
                    return finalize(decode_status::needs_more_input);
                }
                finchar_ = static_cast<u8>(code_);
                oldcode_ = code_;
                state_ = state::OUTPUT_FIRST_CHAR;
                break;
            }

            case state::OUTPUT_FIRST_CHAR: {
                if (out_ptr >= out_end) {
                    return finalize(decode_status::needs_more_output);
                }
                *out_ptr++ = finchar_;
                state_ = state::READ_CODE;
                break;
            }

            case state::READ_CODE: {
                if (!try_read_code(in_ptr, in_end, code_)) {
                    if (input_finished) {
                        state_ = state::DONE;
                        return finalize(decode_status::done);
                    }
                    return finalize(decode_status::needs_more_input);
                }

                if (code_ == END_CODE) {
                    state_ = state::DONE;
                    break;
                }

                if (code_ == CLEAR) {
                    state_ = state::HANDLE_CLEAR;
                    break;
                }

                incode_ = code_;

                // Special case: code not yet in table (KwKwK case)
                if (code_ >= free_ent_) {
                    stack_.push_back(finchar_);
                    code_ = oldcode_;
                }

                state_ = state::BUILD_STRING;
                break;
            }

            case state::HANDLE_CLEAR: {
                init_dictionary();
                if (!try_read_code(in_ptr, in_end, code_)) {
                    if (input_finished) {
                        state_ = state::DONE;
                        return finalize(decode_status::done);
                    }
                    return finalize(decode_status::needs_more_input);
                }
                finchar_ = static_cast<u8>(code_);
                oldcode_ = code_;
                state_ = state::OUTPUT_AFTER_CLEAR;
                break;
            }

            case state::OUTPUT_AFTER_CLEAR: {
                if (out_ptr >= out_end) {
                    return finalize(decode_status::needs_more_output);
                }
                *out_ptr++ = finchar_;
                state_ = state::READ_CODE;
                break;
            }

            case state::BUILD_STRING: {
                // Build output string in reverse
                int safety = 0;
                while (code_ >= 256 && safety < MAX_CODE) {
                    if (code_ < 0 || code_ >= MAX_CODE) {
                        return crate::make_unexpected(error{error_code::CorruptData, "Invalid LZW code"});
                    }
                    stack_.push_back(suffix_[static_cast<size_t>(code_)]);
                    code_ = prefix_[static_cast<size_t>(code_)];
                    safety++;
                }

                if (safety >= MAX_CODE) {
                    return crate::make_unexpected(error{error_code::CorruptData, "LZW decode loop"});
                }

                finchar_ = static_cast<u8>(code_);
                stack_.push_back(finchar_);
                stack_pos_ = stack_.size();
                state_ = state::OUTPUT_STRING;
                break;
            }

            case state::OUTPUT_STRING: {
                // Output in correct order (reverse of stack)
                while (stack_pos_ > 0) {
                    if (out_ptr >= out_end) {
                        return finalize(decode_status::needs_more_output);
                    }
                    *out_ptr++ = stack_[--stack_pos_];
                }
                stack_.clear();
                state_ = state::ADD_TO_DICT;
                break;
            }

            case state::ADD_TO_DICT: {
                // Add to dictionary
                if (free_ent_ < MAX_CODE) {
                    prefix_[static_cast<size_t>(free_ent_)] = static_cast<u16>(oldcode_);
                    suffix_[static_cast<size_t>(free_ent_)] = finchar_;
                    free_ent_++;

                    // Increase bit width if needed
                    if (free_ent_ > max_code_ && n_bits_ < MAX_BITS) {
                        n_bits_++;
                        max_code_ = (1 << n_bits_) - 1;
                    }
                }

                oldcode_ = incode_;
                state_ = state::READ_CODE;
                break;
            }

            case state::DONE:
                break;
        }
    }

    return finalize(decode_status::done);
}

}  // namespace crate
