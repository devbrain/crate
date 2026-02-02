#include <crate/compression/stuffit_rle.hh>

namespace crate {

void stuffit_rle_decompressor::reset() {
    state_ = state::NORMAL;
    last_byte_ = 0;
    repeat_count_ = 0;
}

result_t<stream_result> stuffit_rle_decompressor::decompress_some(
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

    while (true) {
        switch (state_) {
            case state::NORMAL: {
                if (in_ptr >= in_end) {
                    if (input_finished) {
                        return stream_result::done(bytes_read(), bytes_written());
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                byte b = *in_ptr++;

                if (b == ESCAPE_BYTE) {
                    state_ = state::ESCAPE_PENDING;
                } else {
                    if (out_ptr >= out_end) {
                        // Put byte back, need output space
                        in_ptr--;
                        return stream_result::need_output(bytes_read(), bytes_written());
                    }
                    *out_ptr++ = b;
                    last_byte_ = b;
                }
                break;
            }

            case state::ESCAPE_PENDING: {
                if (in_ptr >= in_end) {
                    if (input_finished) {
                        // Truncated escape sequence - treat 0x90 as literal at end
                        if (out_ptr >= out_end) {
                            return stream_result::need_output(bytes_read(), bytes_written());
                        }
                        *out_ptr++ = ESCAPE_BYTE;
                        last_byte_ = ESCAPE_BYTE;
                        state_ = state::NORMAL;
                        return stream_result::done(bytes_read(), bytes_written());
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }

                byte count = *in_ptr++;

                if (count == 0) {
                    // Literal 0x90
                    if (out_ptr >= out_end) {
                        in_ptr--;  // Put count back
                        return stream_result::need_output(bytes_read(), bytes_written());
                    }
                    *out_ptr++ = ESCAPE_BYTE;
                    last_byte_ = ESCAPE_BYTE;
                    state_ = state::NORMAL;
                } else {
                    // Repeat last byte (count - 1) more times
                    // (we already output last_byte_ once before seeing the escape)
                    repeat_count_ = count - 1;
                    if (repeat_count_ > 0) {
                        state_ = state::REPEATING;
                    } else {
                        state_ = state::NORMAL;
                    }
                }
                break;
            }

            case state::REPEATING: {
                while (repeat_count_ > 0) {
                    if (out_ptr >= out_end) {
                        return stream_result::need_output(bytes_read(), bytes_written());
                    }
                    *out_ptr++ = last_byte_;
                    repeat_count_--;
                }
                state_ = state::NORMAL;
                break;
            }
        }
    }
}

}  // namespace crate
