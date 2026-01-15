#include <crate/compression/mszip.hh>

namespace crate {

mszip_decompressor::mszip_decompressor() {
    init_state();
}
result_t<stream_result> mszip_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    // MSZIP decompression requires all input data at once
    if (!input_finished) {
        return stream_result::need_input(0, 0);
    }

    // Check MSZIP signature
    if (input.size() < 2 || input[0] != 'C' || input[1] != 'K') {
        return std::unexpected(error{error_code::InvalidSignature, "Missing MSZIP 'CK' signature"});
    }

    lsb_bitstream bs(input.subspan(2));
    size_t out_pos = 0;

    while (!bs.at_end() && out_pos < output.size()) {
        // Read block header
        auto final_block = bs.read_bit();
        if (!final_block)
            return std::unexpected(final_block.error());

        auto block_type = bs.read_bits(2);
        if (!block_type)
            return std::unexpected(block_type.error());

        switch (*block_type) {
            case 0: {
                // Stored (uncompressed)
                bs.align_to_byte();
                auto len = bs.read_u16_le();
                if (!len)
                    return std::unexpected(len.error());
                auto nlen = bs.read_u16_le();
                if (!nlen)
                    return std::unexpected(nlen.error());

                if ((*len ^ *nlen) != 0xFFFF) {
                    return std::unexpected(error{error_code::CorruptData, "Invalid stored block length"});
                }

                for (u16 i = 0; i < *len && out_pos < output.size(); i++) {
                    auto value = bs.read_byte();
                    if (!value)
                        return std::unexpected(value.error());
                    output[out_pos++] = *value;
                    update_history(*value);
                }
                break;
            }

            case 1: {
                // Fixed Huffman
                build_fixed_tables();
                auto result = decompress_block(bs, output, out_pos);
                if (!result)
                    return std::unexpected(result.error());
                break;
            }

            case 2: {
                // Dynamic Huffman
                auto result = read_dynamic_tables(bs);
                if (!result)
                    return std::unexpected(result.error());
                result = decompress_block(bs, output, out_pos);
                if (!result)
                    return std::unexpected(result.error());
                break;
            }

            default:
                return std::unexpected(error{error_code::InvalidBlockType, "Reserved block type 3"});
        }

        if (*final_block)
            break;
    }

    return stream_result::done(input.size(), out_pos);
}
void mszip_decompressor::reset() {
    init_state();
}
void mszip_decompressor::init_state() {
    history_pos_ = 0;
    std::fill(history_.begin(), history_.end(), 0);
}
void mszip_decompressor::update_history(u8 value) {
    history_[history_pos_++ & (MSZIP_BLOCK_SIZE - 1)] = value;
}
u8 mszip_decompressor::get_history(size_t distance) const {
    return history_[(history_pos_ - distance) & (MSZIP_BLOCK_SIZE - 1)];
}
void mszip_decompressor::build_fixed_tables() {
    // Fixed literal/length code lengths (RFC 1951)
    std::array<u8, 288> lit_lengths{};
    std::fill(lit_lengths.begin(), lit_lengths.begin() + 144, 8);
    std::fill(lit_lengths.begin() + 144, lit_lengths.begin() + 256, 9);
    std::fill(lit_lengths.begin() + 256, lit_lengths.begin() + 280, 7);
    std::fill(lit_lengths.begin() + 280, lit_lengths.end(), 8);
    literal_decoder_.build(lit_lengths);

    // Fixed distance code lengths
    std::array<u8, 32> dist_lengths{};
    std::fill(dist_lengths.begin(), dist_lengths.end(), 5);
    distance_decoder_.build(dist_lengths);
}
void_result_t mszip_decompressor::read_dynamic_tables(lsb_bitstream& bs) {
    auto hlit = bs.read_bits(5);
    if (!hlit)
        return std::unexpected(hlit.error());
    auto hdist = bs.read_bits(5);
    if (!hdist)
        return std::unexpected(hdist.error());
    auto hclen = bs.read_bits(4);
    if (!hclen)
        return std::unexpected(hclen.error());

    unsigned num_lit = *hlit + 257;
    unsigned num_dist = *hdist + 1;
    unsigned num_code_len = *hclen + 4;

    // Read code length code lengths
    std::array<u8, 19> code_len_lengths{};
    for (unsigned i = 0; i < num_code_len; i++) {
        auto len = bs.read_bits(3);
        if (!len)
            return std::unexpected(len.error());
        code_len_lengths[deflate::code_length_order[i]] = static_cast<u8>(*len);
    }

    huffman_decoder<19> code_len_decoder;
    auto result = code_len_decoder.build(code_len_lengths);
    if (!result)
        return result;

    // Decode literal and distance code lengths
    std::vector<u8> all_lengths(num_lit + num_dist);
    size_t i = 0;
    u8 last_len = 0;

    while (i < all_lengths.size()) {
        auto sym = code_len_decoder.decode(bs);
        if (!sym)
            return std::unexpected(sym.error());

        if (*sym < 16) {
            all_lengths[i++] = static_cast<u8>(*sym);
            last_len = static_cast<u8>(*sym);
        } else if (*sym == 16) {
            auto repeat = bs.read_bits(2);
            if (!repeat)
                return std::unexpected(repeat.error());
            for (unsigned j = 0; j < *repeat + 3 && i < all_lengths.size(); j++) {
                all_lengths[i++] = last_len;
            }
        } else if (*sym == 17) {
            auto repeat = bs.read_bits(3);
            if (!repeat)
                return std::unexpected(repeat.error());
            for (unsigned j = 0; j < *repeat + 3 && i < all_lengths.size(); j++) {
                all_lengths[i++] = 0;
            }
        } else {
            // 18
            auto repeat = bs.read_bits(7);
            if (!repeat)
                return std::unexpected(repeat.error());
            for (unsigned j = 0; j < *repeat + 11 && i < all_lengths.size(); j++) {
                all_lengths[i++] = 0;
            }
        }
    }

    // Build tables
    auto lit_result = literal_decoder_.build(std::span(all_lengths.data(), num_lit));
    if (!lit_result)
        return lit_result;

    return distance_decoder_.build(std::span(all_lengths.data() + num_lit, num_dist));
}
void_result_t mszip_decompressor::decompress_block(lsb_bitstream& bs, mutable_byte_span output, size_t& out_pos) {
    while (out_pos < output.size()) {
        auto sym = literal_decoder_.decode(bs);
        if (!sym)
            return std::unexpected(sym.error());

        if (*sym < 256) {
            // Literal byte
            output[out_pos++] = static_cast<u8>(*sym);
            update_history(static_cast<u8>(*sym));
        } else if (*sym == 256) {
            // End of block
            break;
        } else {
            // Length-distance pair
            unsigned len_idx = *sym - 257;
            if (len_idx >= deflate::length_base.size()) {
                return std::unexpected(error{error_code::InvalidMatchLength});
            }

            auto extra_bits = bs.read_bits(deflate::length_extra_bits[len_idx]);
            if (!extra_bits)
                return std::unexpected(extra_bits.error());
            unsigned length = deflate::length_base[len_idx] + *extra_bits;

            auto dist_sym = distance_decoder_.decode(bs);
            if (!dist_sym)
                return std::unexpected(dist_sym.error());

            if (*dist_sym >= deflate::distance_base.size()) {
                return std::unexpected(error{error_code::InvalidMatchDistance});
            }

            auto dist_extra = bs.read_bits(deflate::distance_extra_bits[*dist_sym]);
            if (!dist_extra)
                return std::unexpected(dist_extra.error());
            unsigned distance = deflate::distance_base[*dist_sym] + *dist_extra;

            // Copy match
            for (unsigned i = 0; i < length && out_pos < output.size(); i++) {
                u8 value = get_history(distance);
                output[out_pos++] = value;
                update_history(value);
            }
        }
    }
    return {};
}
}  // namespace crate
