#include <crate/compression/lzss.hh>

namespace crate {

szdd_lzss_decompressor::szdd_lzss_decompressor() {
    init_state();
}
result_t<size_t> szdd_lzss_decompressor::decompress(byte_span input, mutable_byte_span output) {
    size_t in_pos = 0;
    size_t out_pos = 0;

    while (in_pos < input.size() && out_pos < output.size()) {
        if (in_pos >= input.size())
            break;
        u8 control = input[in_pos++];

        for (unsigned bit = 0; bit < 8 && out_pos < output.size(); bit++) {
            if (control & (1 << bit)) {
                // Literal byte
                if (in_pos >= input.size())
                    break;
                u8 value = input[in_pos++];
                output[out_pos++] = value;
                window_[window_pos_++ & WINDOW_MASK] = value;
            } else {
                // Match reference
                if (in_pos + 1 >= input.size())
                    break;

                u8 lo = input[in_pos++];
                u8 hi = input[in_pos++];

                unsigned match_pos = lo | ((hi & 0xF0) << 4);
                unsigned match_len = (hi & 0x0F) + MIN_MATCH;

                for (unsigned i = 0; i < match_len && out_pos < output.size(); i++) {
                    u8 value = window_[(match_pos + i) & WINDOW_MASK];
                    output[out_pos++] = value;
                    window_[window_pos_++ & WINDOW_MASK] = value;
                }
            }
        }
    }

    return out_pos;
}
void szdd_lzss_decompressor::reset() {
    init_state();
}
void szdd_lzss_decompressor::init_state() {
    window_pos_ = INITIAL_POS;
    std::fill(window_.begin(), window_.end(), ' ');
}
}  // namespace crate
