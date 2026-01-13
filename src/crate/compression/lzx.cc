#include <crate/compression/lzx.hh>

namespace crate {

result_t<std::unique_ptr<lzx_decompressor>> lzx_decompressor::create(unsigned window_bits) {
    if (window_bits < lzx::MIN_WINDOW_BITS || window_bits > lzx::MAX_WINDOW_BITS) {
        return std::unexpected(error{error_code::InvalidParameter, "LZX window_bits must be 15-21"});
    }
    return std::make_unique<lzx_decompressor>(window_bits);
}
lzx_decompressor::lzx_decompressor(unsigned window_bits)
    : window_bits_(window_bits),
      window_size_(1u << window_bits),
      window_(window_size_, 0),
      num_position_slots_(calculate_position_slots(window_bits)) {
    init_state();
}
result_t<unsigned long> lzx_decompressor::decompress(byte_span input, mutable_byte_span output) {
    msb_bitstream bs(input);
    size_t out_pos = 0;
    size_t remaining = output.size();

    while (remaining > 0) {
        // Read block type and size
        auto block_type = bs.read_bits(3);
        if (!block_type) {
            if (out_pos > 0)
                break;  // EOF after some data is ok
            return std::unexpected(block_type.error());
        }

        auto block_size_hi = bs.read_bits(8);
        if (!block_size_hi)
            return std::unexpected(block_size_hi.error());
        auto block_size_lo = bs.read_bits(8);
        if (!block_size_lo)
            return std::unexpected(block_size_lo.error());

        size_t block_size = (*block_size_hi << 8) | *block_size_lo;
        if (block_size == 0)
            block_size = 32768;

        block_size = std::min(block_size, remaining);

        switch (*block_type) {
            case lzx::BLOCKTYPE_ALIGNED: {
                // Read aligned offset tree
                auto result = read_aligned_tree(bs);
                if (!result)
                    return std::unexpected(result.error());
                [[fallthrough]];
            }

            case lzx::BLOCKTYPE_VERBATIM: {
                auto result = read_main_and_length_trees(bs);
                if (!result)
                    return std::unexpected(result.error());

                result = decompress_block(bs, output, out_pos, block_size, *block_type == lzx::BLOCKTYPE_ALIGNED);
                if (!result)
                    return std::unexpected(result.error());
                break;
            }

            case lzx::BLOCKTYPE_UNCOMPRESSED: {
                bs.align_to_byte();

                // Read R0, R1, R2
                auto r0 = bs.read_u32_le();
                if (!r0)
                    return std::unexpected(r0.error());
                auto r1 = bs.read_u32_le();
                if (!r1)
                    return std::unexpected(r1.error());
                auto r2 = bs.read_u32_le();
                if (!r2)
                    return std::unexpected(r2.error());

                R0_ = *r0;
                R1_ = *r1;
                R2_ = *r2;

                // Copy uncompressed data
                for (size_t i = 0; i < block_size; i++) {
                    auto value = bs.read_byte();
                    if (!value)
                        return std::unexpected(value.error());
                    output[out_pos++] = *value;
                    window_[window_pos_++ & (window_size_ - 1)] = *value;
                }

                // Re-align if odd block size
                if (block_size & 1) {
                    bs.read_byte();
                }
                break;
            }

            default:
                return std::unexpected(error{error_code::InvalidBlockType, "Invalid LZX block type"});
        }

        remaining -= block_size;
    }

    return out_pos;
}
void lzx_decompressor::reset() {
    init_state();
}
void lzx_decompressor::init_state() {
    R0_ = 1;
    R1_ = 1;
    R2_ = 1;
    window_pos_ = 0;
    std::fill(window_.begin(), window_.end(), 0);
    std::fill(main_lengths_.begin(), main_lengths_.end(), 0);
    std::fill(length_lengths_.begin(), length_lengths_.end(), 0);
}
unsigned lzx_decompressor::calculate_position_slots(unsigned window_bits) {
    if (window_bits < 15)
        return 2 * window_bits;
    if (window_bits < 17)
        return 32 + 2 * (window_bits - 15);
    return 36 + 2 * (window_bits - 17);
}
void_result_t lzx_decompressor::read_aligned_tree(msb_bitstream& bs) {
    std::array<u8, lzx::NUM_ALIGNED_SYMBOLS> lengths{};
    for (unsigned i = 0; i < lzx::NUM_ALIGNED_SYMBOLS; i++) {
        auto len = bs.read_bits(3);
        if (!len)
            return std::unexpected(len.error());
        lengths[i] = static_cast<u8>(*len);
    }
    return aligned_decoder_.build(lengths);
}
void_result_t lzx_decompressor::read_main_and_length_trees(msb_bitstream& bs) {
    // Read pretree for main tree (first 256 symbols)
    auto result = read_lengths_with_pretree(bs, main_lengths_, 0, lzx::NUM_CHARS);
    if (!result)
        return result;

    // Read pretree for main tree (remaining symbols)
    size_t main_tree_size = lzx::NUM_CHARS + num_position_slots_ * 8;
    result = read_lengths_with_pretree(bs, main_lengths_, lzx::NUM_CHARS, main_tree_size);
    if (!result)
        return result;

    // Build main tree
    result = main_decoder_.build(std::span(main_lengths_.data(), main_tree_size));
    if (!result)
        return result;

    // Read length tree
    result = read_lengths_with_pretree(bs, length_lengths_, 0, lzx::NUM_SECONDARY_LENGTHS);
    if (!result)
        return result;

    return length_decoder_.build(length_lengths_);
}
void_result_t lzx_decompressor::read_lengths_with_pretree(msb_bitstream& bs, std::span<u8> lengths, size_t start,
                                                          size_t end) {
    // Read pretree
    std::array<u8, 20> pretree_lengths{};
    for (unsigned i = 0; i < 20; i++) {
        auto len = bs.read_bits(4);
        if (!len)
            return std::unexpected(len.error());
        pretree_lengths[i] = static_cast<u8>(*len);
    }

    huffman_decoder<20> pretree;
    auto result = pretree.build(pretree_lengths);
    if (!result)
        return result;

    // Decode lengths
    for (size_t i = start; i < end;) {
        auto sym = pretree.decode(bs);
        if (!sym)
            return std::unexpected(sym.error());

        if (*sym < 17) {
            lengths[i] = static_cast<u8>((lengths[i] + 17 - *sym) % 17);
            i++;
        } else if (*sym == 17) {
            auto run = bs.read_bits(4);
            if (!run)
                return std::unexpected(run.error());
            for (unsigned j = 0; j < *run + 4 && i < end; j++) {
                lengths[i++] = 0;
            }
        } else if (*sym == 18) {
            auto run = bs.read_bits(5);
            if (!run)
                return std::unexpected(run.error());
            for (unsigned j = 0; j < *run + 20 && i < end; j++) {
                lengths[i++] = 0;
            }
        } else {  // 19
            auto run = bs.read_bits(1);
            if (!run)
                return std::unexpected(run.error());
            auto next = pretree.decode(bs);
            if (!next)
                return std::unexpected(next.error());

            u8 len = static_cast<u8>((lengths[i] + 17 - *next) % 17);
            for (unsigned j = 0; j < *run + 4 && i < end; j++) {
                lengths[i++] = len;
            }
        }
    }

    return {};
}
void_result_t lzx_decompressor::decompress_block(msb_bitstream& bs, mutable_byte_span output, size_t& out_pos,
                                                 size_t block_size, bool use_aligned) {
    size_t block_end = out_pos + block_size;

    while (out_pos < block_end) {
        auto main_sym = main_decoder_.decode(bs);
        if (!main_sym)
            return std::unexpected(main_sym.error());

        if (*main_sym < lzx::NUM_CHARS) {
            // Literal
            output[out_pos++] = static_cast<u8>(*main_sym);
            window_[window_pos_++ & (window_size_ - 1)] = static_cast<u8>(*main_sym);
        } else {
            // Match
            unsigned match_sym = *main_sym - lzx::NUM_CHARS;
            unsigned position_slot = match_sym / 8;
            unsigned length_header = match_sym % 8;

            // Get match length
            unsigned match_length;
            if (length_header == 7) {
                auto len_sym = length_decoder_.decode(bs);
                if (!len_sym)
                    return std::unexpected(len_sym.error());
                match_length = *len_sym + lzx::NUM_PRIMARY_LENGTHS + lzx::MIN_MATCH;
            } else {
                match_length = length_header + lzx::MIN_MATCH;
            }

            // Get match offset
            u32 match_offset;
            if (position_slot == 0) {
                match_offset = R0_;
            } else if (position_slot == 1) {
                match_offset = R1_;
                std::swap(R0_, R1_);
            } else if (position_slot == 2) {
                match_offset = R2_;
                std::swap(R0_, R2_);
            } else {
                unsigned extra = lzx::extra_bits[position_slot];
                u32 verbatim_bits = 0;
                u32 aligned_bits = 0;

                if (use_aligned && extra >= 3) {
                    auto v = bs.read_bits(extra - 3);
                    if (!v)
                        return std::unexpected(v.error());
                    verbatim_bits = *v << 3;

                    auto a = aligned_decoder_.decode(bs);
                    if (!a)
                        return std::unexpected(a.error());
                    aligned_bits = *a;
                } else if (extra > 0) {
                    auto v = bs.read_bits(extra);
                    if (!v)
                        return std::unexpected(v.error());
                    verbatim_bits = *v;
                }

                match_offset = lzx::position_base[position_slot] + verbatim_bits + aligned_bits;
                R2_ = R1_;
                R1_ = R0_;
                R0_ = match_offset;
            }

            // Copy match
            for (unsigned i = 0; i < match_length && out_pos < block_end; i++) {
                u8 value = window_[(window_pos_ - match_offset) & (window_size_ - 1)];
                output[out_pos++] = value;
                window_[window_pos_++ & (window_size_ - 1)] = value;
            }
        }
    }

    return {};
}
}  // namespace crate