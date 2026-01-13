#pragma once

#include <algorithm>
#include <array>
#include <crate/core/decompressor.hh>
#include <crate/core/bitstream.hh>
#include <crate/core/types.hh>
#include <vector>

namespace crate {

// LZH format variant
enum class lzh_format {
    LH5,  // 8KB window
    LH6,  // 32KB window (used by ARJ method 1-3)
    LH7   // 64KB window (used by ARJZ)
};

// Huffman tree for LZH decompression
class CRATE_EXPORT lzh_huffman_tree {
public:
    static constexpr unsigned MAX_CODES = 512;
    static constexpr unsigned MAX_CODE_LENGTH = 16;

    lzh_huffman_tree() = default;

    void clear();

    bool build_from_lengths(const std::vector<unsigned>& lengths);

    // Build a tree with a single value (for ncodes=0 case)
    void build_single(unsigned value);

    // Decode a symbol from the bitstream
    template<typename Bitstream>
    result_t<unsigned> decode(Bitstream& bs) {
        if (is_single_) {
            return single_value_;
        }

        // Try lookup table first (8 bits)
        auto peek = bs.peek_bits(8);
        if (!peek)
            return std::unexpected(peek.error());

        u16 entry = lookup_[*peek];
        if (entry != 0xFFFF) {
            unsigned len = entry >> 12;
            unsigned value = entry & 0x0FFF;
            bs.remove_bits(len);
            return value;
        }

        // Slow path: bit-by-bit decoding
        unsigned code = 0;
        for (unsigned len = 1; len <= max_length_; len++) {
            auto bit = bs.read_bit();
            if (!bit)
                return std::unexpected(bit.error());
            code = (code << 1) | (*bit ? 1 : 0);

            // Search for matching code
            for (unsigned i = 0; i < codes_.size(); i++) {
                if (lengths_[i] == len && codes_[i] == code) {
                    return i;
                }
            }
        }

        return std::unexpected(error{error_code::InvalidHuffmanTable, "Failed to decode Huffman symbol"});
    }

private:
    void build_lookup_table();

    std::vector<unsigned> codes_;
    std::vector<unsigned> lengths_;
    std::array<u16, 256> lookup_{};
    unsigned max_length_ = 0;
    unsigned single_value_ = 0;
    bool is_single_ = false;
};

// LZH (LH5/LH6/LH7) decompressor
// Used by ARJ methods 1-3 and LHA archives
class CRATE_EXPORT lzh_decompressor : public decompressor {
public:
    explicit lzh_decompressor(lzh_format format = lzh_format::LH6);

    result_t<size_t> decompress(byte_span input, mutable_byte_span output) override;

    void reset() override;

private:
    void init_state();
    // Read a code length (3 bits + optional extension)
    template<typename Bitstream>
    result_t<unsigned> read_code_length(Bitstream& bs) {
        auto n = bs.read_bits(3);
        if (!n)
            return std::unexpected(n.error());

        unsigned len = *n;
        if (len == 7) {
            while (true) {
                auto bit = bs.read_bit();
                if (!bit)
                    return std::unexpected(bit.error());
                if (!*bit)
                    break;
                len++;
                if (len > 16) {
                    return std::unexpected(error{error_code::InvalidHuffmanTable, "Code length too large"});
                }
            }
        }
        return len;
    }

    // Read code lengths tree (for decoding other trees)
    template<typename Bitstream>
    result_t<bool> read_codelengths_tree(Bitstream& bs) {
        auto ncodes_result = bs.read_bits(5);
        if (!ncodes_result)
            return std::unexpected(ncodes_result.error());

        unsigned ncodes = *ncodes_result;
        if (ncodes > 20)
            ncodes = 20;

        if (ncodes == 0) {
            // Single value tree
            auto val = bs.read_bits(5);
            if (!val)
                return std::unexpected(val.error());
            codelengths_tree_.build_single(*val);
            return true;
        }

        std::vector<unsigned> lengths(ncodes, 0);
        for (unsigned i = 0; i < ncodes; i++) {
            auto len = read_code_length(bs);
            if (!len)
                return std::unexpected(len.error());
            lengths[i] = *len;

            // After first 3 lengths, read optional skip
            if (i == 2) {
                auto skip = bs.read_bits(2);
                if (!skip)
                    return std::unexpected(skip.error());
                i += *skip;
            }
        }

        if (!codelengths_tree_.build_from_lengths(lengths)) {
            return std::unexpected(error{error_code::InvalidHuffmanTable, "Failed to build code lengths tree"});
        }
        return true;
    }

    // Read skip length (for sparse trees)
    template<typename Bitstream>
    result_t<unsigned> read_skip_length(Bitstream& bs, unsigned code) {
        if (code == 0)
            return 0u;
        if (code == 1) {
            auto extra = bs.read_bits(4);
            if (!extra)
                return std::unexpected(extra.error());
            return 2 + *extra;
        }
        // code == 2
        auto extra = bs.read_bits(9);
        if (!extra)
            return std::unexpected(extra.error());
        return 19 + *extra;
    }

    // Read literals/lengths tree
    template<typename Bitstream>
    result_t<bool> read_literals_tree(Bitstream& bs) {
        auto ncodes_result = bs.read_bits(9);
        if (!ncodes_result)
            return std::unexpected(ncodes_result.error());

        unsigned ncodes = *ncodes_result;
        if (ncodes > 510) {
            return std::unexpected(error{error_code::InvalidHuffmanTable, "Too many literal codes"});
        }

        if (ncodes == 0) {
            auto val = bs.read_bits(9);
            if (!val)
                return std::unexpected(val.error());
            if (*val >= 510) {
                return std::unexpected(error{error_code::InvalidHuffmanTable});
            }
            literals_tree_.build_single(*val);
            return true;
        }

        std::vector<unsigned> lengths(ncodes, 0);
        unsigned i = 0;
        while (i < ncodes) {
            auto code = codelengths_tree_.decode(bs);
            if (!code)
                return std::unexpected(code.error());

            if (*code <= 2) {
                auto skip = read_skip_length(bs, *code);
                if (!skip)
                    return std::unexpected(skip.error());
                i += 1 + *skip;
            } else {
                lengths[i] = *code - 2;
                i++;
            }
        }

        if (!literals_tree_.build_from_lengths(lengths)) {
            return std::unexpected(error{error_code::InvalidHuffmanTable, "Failed to build literals tree"});
        }
        return true;
    }

    // Read offsets tree
    template<typename Bitstream>
    result_t<bool> read_offsets_tree(Bitstream& bs) {
        auto ncodes_result = bs.read_bits(offsets_bits_);
        if (!ncodes_result)
            return std::unexpected(ncodes_result.error());

        unsigned ncodes = *ncodes_result;
        if (ncodes > max_offset_codes_) {
            return std::unexpected(error{error_code::InvalidHuffmanTable, "Too many offset codes"});
        }

        if (ncodes == 0) {
            auto val = bs.read_bits(offsets_bits_);
            if (!val)
                return std::unexpected(val.error());
            if (*val >= max_offset_codes_) {
                return std::unexpected(error{error_code::InvalidHuffmanTable});
            }
            offsets_tree_.build_single(*val);
            return true;
        }

        std::vector<unsigned> lengths(ncodes, 0);
        for (unsigned i = 0; i < ncodes; i++) {
            auto len = read_code_length(bs);
            if (!len)
                return std::unexpected(len.error());
            lengths[i] = *len;
        }

        if (!offsets_tree_.build_from_lengths(lengths)) {
            return std::unexpected(error{error_code::InvalidHuffmanTable, "Failed to build offsets tree"});
        }
        return true;
    }

    // Process a single block
    template<typename Bitstream>
    result_t<size_t> process_block(Bitstream& bs, mutable_byte_span output, size_t out_pos) {
        // Read number of codes in this block
        auto ncodes_result = bs.read_bits(16);
        if (!ncodes_result)
            return std::unexpected(ncodes_result.error());

        unsigned ncodes = *ncodes_result;
        if (ncodes == 0) {
            // Zero means 65536 codes (ARJ extension)
            ncodes = 65536;
        }

        // Read trees
        auto r = read_codelengths_tree(bs);
        if (!r)
            return std::unexpected(r.error());

        r = read_literals_tree(bs);
        if (!r)
            return std::unexpected(r.error());

        r = read_offsets_tree(bs);
        if (!r)
            return std::unexpected(r.error());

        // Decode codes
        for (unsigned i = 0; i < ncodes && out_pos < output.size(); i++) {
            auto code = literals_tree_.decode(bs);
            if (!code) {
                if (out_pos > 0)
                    return out_pos;
                return std::unexpected(code.error());
            }

            if (*code < 256) {
                // Literal byte
                u8 value = static_cast<u8>(*code);
                output[out_pos++] = value;
                window_[window_pos_++ % window_size_] = value;
            } else {
                // Match: code 256-509 represents length 3-256
                unsigned length = *code - 253;

                // Decode offset
                auto ocode = offsets_tree_.decode(bs);
                if (!ocode) {
                    if (out_pos > 0)
                        return out_pos;
                    return std::unexpected(ocode.error());
                }

                unsigned offset;
                if (*ocode <= 1) {
                    offset = *ocode;
                } else {
                    unsigned extra_bits = *ocode - 1;
                    auto extra = bs.read_bits(extra_bits);
                    if (!extra) {
                        if (out_pos > 0)
                            return out_pos;
                        return std::unexpected(extra.error());
                    }
                    offset = (1u << extra_bits) + *extra;
                }

                // Copy from history
                for (unsigned j = 0; j < length && out_pos < output.size(); j++) {
                    size_t src_pos = (window_pos_ - offset - 1 + window_size_) % window_size_;
                    u8 value = window_[src_pos];
                    output[out_pos++] = value;
                    window_[window_pos_++ % window_size_] = value;
                }
            }
        }

        return out_pos;
    }

    lzh_format format_ = lzh_format::LH5;
    size_t window_size_ = 0;
    unsigned offsets_bits_ = 0;
    unsigned max_offset_codes_ = 0;

    std::vector<u8> window_;
    size_t window_pos_ = 0;

    lzh_huffman_tree codelengths_tree_;
    lzh_huffman_tree literals_tree_;
    lzh_huffman_tree offsets_tree_;
};

}  // namespace crate
