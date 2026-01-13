#include <crate/compression/arj_lz.hh>

namespace crate {

arj_method4_decompressor::arj_method4_decompressor(bool old_format) : old_format_(old_format) {
    init_state();
}

result_t<size_t> arj_method4_decompressor::decompress(byte_span input, mutable_byte_span output) {
    msb_bitstream bs(input);  // ARJ uses MSB-first bit ordering
    size_t out_pos = 0;

    while (!bs.at_end() && out_pos < output.size()) {
        // Read length code
        auto len_code = read_length_code(bs);
        if (!len_code) {
            if (out_pos > 0)
                break;  // EOF is OK if we have data
            return std::unexpected(len_code.error());
        }

        if (*len_code == 0) {
            // Literal byte
            auto value = bs.read_bits(8);
            if (!value) {
                if (out_pos > 0)
                    break;
                return std::unexpected(value.error());
            }
            u8 b = static_cast<u8>(*value);
            output[out_pos++] = b;
            window_[window_pos_++ & WINDOW_MASK] = b;
        } else {
            // Match reference: length = len_code + 2
            unsigned match_len = *len_code + 2;

            auto offset = read_offset(bs);
            if (!offset) {
                if (out_pos > 0)
                    break;
                return std::unexpected(offset.error());
            }

            // Copy from history buffer
            // offset+1 is the distance back in the window
            for (unsigned i = 0; i < match_len && out_pos < output.size(); i++) {
                u8 value = window_[(window_pos_ - *offset - 1) & WINDOW_MASK];
                output[out_pos++] = value;
                window_[window_pos_++ & WINDOW_MASK] = value;
            }
        }
    }

    return out_pos;
}
void arj_method4_decompressor::reset() {
    init_state();
}
void arj_method4_decompressor::init_state() {
    window_pos_ = 0;
    std::fill(window_.begin(), window_.end(), 0);
}
result_t<unsigned> arj_method4_decompressor::read_length_code(msb_bitstream& bs) const {
    unsigned ones_count = 0;

    // Read up to 7 bits, counting consecutive 1s, stopping after first 0
    while (ones_count < 7) {
        auto bit = bs.read_bit();
        if (!bit)
            return std::unexpected(bit.error());
        if (!*bit)
            break;
        ones_count++;
    }

    // Old format (ARJ v0.13-0.14) has an extra bit after 7 ones
    if (ones_count >= 7 && old_format_) {
        auto extra = bs.read_bit();
        if (!extra)
            return std::unexpected(extra.error());
        // The extra bit is presumed to be 0
    }

    if (ones_count == 0)
        return 0u;

    // Read 'ones_count' more bits for the value
    auto value_bits = bs.read_bits(ones_count);
    if (!value_bits)
        return std::unexpected(value_bits.error());

    // Result = (1 << ones_count) - 1 + value_bits
    return ((1u << ones_count) - 1) + *value_bits;
}
result_t<unsigned> arj_method4_decompressor::read_offset(msb_bitstream& bs) const {
    unsigned ones_count = 0;

    // Read up to 4 bits, counting consecutive 1s, stopping after first 0
    while (ones_count < 4) {
        auto bit = bs.read_bit();
        if (!bit)
            return std::unexpected(bit.error());
        if (!*bit)
            break;
        ones_count++;
    }

    // Old format has an extra bit after 4 ones
    if (ones_count >= 4 && old_format_) {
        auto extra = bs.read_bit();
        if (!extra)
            return std::unexpected(extra.error());
    }

    // Read (9 + ones_count) more bits
    auto value_bits = bs.read_bits(9 + ones_count);
    if (!value_bits)
        return std::unexpected(value_bits.error());

    // Result = (1 << (9 + ones_count)) - 512 + value_bits
    return ((1u << (9 + ones_count)) - 512) + *value_bits;
}
}  // namespace crate
