#pragma once

#include <crate/core/types.hh>
#include <crate/core/error.hh>
#include <crate/core/bitstream.hh>
#include <array>
#include <algorithm>

namespace crate {
    // Maximum code length for Huffman codes
    constexpr unsigned MAX_CODE_LENGTH = 16;
    constexpr unsigned HUFFMAN_TABLE_BITS = 10; // Fast lookup table size

    // Huffman decode table entry
    struct CRATE_EXPORT huffman_entry {
        u16 symbol = 0; // Decoded symbol
        u8 length = 0; // Code length
        u8 flags = 0; // 0 = normal, 1 = needs subtable lookup
    };

    // Generic Huffman decoder
    template<size_t MaxSymbols, unsigned TableBits = HUFFMAN_TABLE_BITS>
    class CRATE_EXPORT huffman_decoder {
        public:
            static constexpr size_t TABLE_SIZE = 1u << TableBits;

            huffman_decoder() = default;

            // Build decode table from code lengths
            result_t <void> build(std::span <const u8> lengths) {
                if (lengths.size() > MaxSymbols) {
                    return std::unexpected(error{
                        error_code::InvalidHuffmanTable,
                        "Too many symbols"
                    });
                }

                // Reset state
                std::fill(table_.begin(), table_.end(), huffman_entry{0, 0, 0});
                overflow_count_ = 0;
                max_length_ = 0;

                // Count codes of each length
                std::array <unsigned, MAX_CODE_LENGTH + 1> count{};
                for (auto len : lengths) {
                    if (len > MAX_CODE_LENGTH) {
                        return std::unexpected(error{
                            error_code::InvalidHuffmanTable,
                            "Code length too large"
                        });
                    }
                    if (len > 0) count[len]++;
                }

                // Find max length
                for (unsigned i = MAX_CODE_LENGTH; i > 0; i--) {
                    if (count[i] > 0) {
                        max_length_ = static_cast <u8>(i);
                        break;
                    }
                }

                if (max_length_ == 0) {
                    // Empty table is valid
                    return {};
                }

                // Generate codes for each length
                std::array <u32, MAX_CODE_LENGTH + 1> next_code{};
                u32 code = 0;
                for (unsigned bits = 1; bits <= MAX_CODE_LENGTH; bits++) {
                    code = (code + count[bits - 1]) << 1;
                    next_code[bits] = code;
                }

                // Fill tables
                for (size_t sym = 0; sym < lengths.size(); sym++) {
                    u8 len = lengths[sym];
                    if (len == 0) continue;

                    u32 c = next_code[len]++;

                    if (len <= TableBits) {
                        // Fits in fast table - replicate entry
                        u32 fill = 1u << (TableBits - len);
                        u32 idx = reverse_bits(c, len);
                        for (u32 i = 0; i < fill; i++) {
                            table_[idx | (i << len)] = huffman_entry{
                                static_cast <u16>(sym), len, 0
                            };
                        }
                    } else {
                        // Needs secondary table lookup (store in overflow)
                        if (overflow_count_ < overflow_.size()) {
                            overflow_[overflow_count_++] = {
                                static_cast <u16>(sym), len, c
                            };
                        }
                    }
                }

                return {};
            }

            // Decode one symbol from LSB-first bitstream
            result_t <u16> decode(lsb_bitstream& bs) {
                auto peek_result = bs.peek_bits(std::min <unsigned>(max_length_, TableBits));
                if (!peek_result) return std::unexpected(peek_result.error());

                u32 bits = *peek_result;

                // Fast path: lookup in main table
                const auto& entry = table_[bits & (TABLE_SIZE - 1)];

                if (entry.length > 0 && entry.length <= TableBits) {
                    bs.remove_bits(entry.length);
                    return entry.symbol;
                }

                // Slow path: search overflow table
                for (size_t i = 0; i < overflow_count_; i++) {
                    const auto& ov = overflow_[i];
                    auto read_result = bs.peek_bits(ov.length);
                    if (!read_result) continue;

                    u32 code = reverse_bits(*read_result, ov.length);
                    if (code == ov.code) {
                        bs.remove_bits(ov.length);
                        return ov.symbol;
                    }
                }

                return std::unexpected(error{
                    error_code::InvalidHuffmanTable,
                    "Failed to decode symbol"
                });
            }

            // Decode one symbol from MSB-first bitstream
            result_t <u16> decode(msb_bitstream& bs) {
                auto peek_result = bs.peek_bits(std::min <unsigned>(max_length_, TableBits));
                if (!peek_result) return std::unexpected(peek_result.error());

                u32 bits = *peek_result;

                // For MSB, reverse bits for table lookup
                u32 idx = reverse_bits(bits, TableBits);
                const auto& entry = table_[idx & (TABLE_SIZE - 1)];

                if (entry.length > 0 && entry.length <= TableBits) {
                    bs.remove_bits(entry.length);
                    return entry.symbol;
                }

                // Slow path
                for (size_t i = 0; i < overflow_count_; i++) {
                    const auto& ov = overflow_[i];
                    auto read_result = bs.peek_bits(ov.length);
                    if (!read_result) continue;

                    if (*read_result == ov.code) {
                        bs.remove_bits(ov.length);
                        return ov.symbol;
                    }
                }

                return std::unexpected(error{
                    error_code::InvalidHuffmanTable,
                    "Failed to decode symbol"
                });
            }

            // Try to decode one symbol from a streaming MSB reader
            // Reader must provide: bool try_peek_bits(unsigned, u32&), void remove_bits(unsigned)
            template <typename Reader>
            result_t <bool> try_decode_msb(Reader& reader, u16& out) {
                if (max_length_ == 0) {
                    return std::unexpected(error{
                        error_code::InvalidHuffmanTable,
                        "Failed to decode symbol"
                    });
                }

                u32 bits = 0;
                unsigned peek_len = std::min <unsigned>(max_length_, TableBits);
                if (!reader.try_peek_bits(peek_len, bits)) {
                    return false;
                }

                u32 idx = reverse_bits(bits, TableBits);
                const auto& entry = table_[idx & (TABLE_SIZE - 1)];

                if (entry.length > 0 && entry.length <= TableBits) {
                    reader.remove_bits(entry.length);
                    out = entry.symbol;
                    return true;
                }

                for (size_t i = 0; i < overflow_count_; i++) {
                    const auto& ov = overflow_[i];
                    if (!reader.try_peek_bits(ov.length, bits)) {
                        return false;
                    }
                    if (bits == ov.code) {
                        reader.remove_bits(ov.length);
                        out = ov.symbol;
                        return true;
                    }
                }

                return std::unexpected(error{
                    error_code::InvalidHuffmanTable,
                    "Failed to decode symbol"
                });
            }

        private:
            static constexpr u32 reverse_bits(u32 v, unsigned n) {
                u32 result = 0;
                for (unsigned i = 0; i < n; i++) {
                    result = (result << 1) | (v & 1);
                    v >>= 1;
                }
                return result;
            }

            std::array <huffman_entry, TABLE_SIZE> table_{};

            struct CRATE_EXPORT OverflowEntry {
                u16 symbol = 0;
                u8 length = 0;
                u32 code = 0;
            };

            std::array <OverflowEntry, MaxSymbols> overflow_{};
            size_t overflow_count_ = 0;
            u8 max_length_ = 0;
    };

    // Specialized decoders for common cases
    using literal_decoder = huffman_decoder <288>; // Deflate literal/length
    using distance_decoder = huffman_decoder <32>; // Deflate distance
    using lzx_main_decoder = huffman_decoder <720>; // LZX main tree
    using lzx_length_decoder = huffman_decoder <249>; // LZX length tree
    using lzx_aligned_decoder = huffman_decoder <8>; // LZX aligned offset
} // namespace crate
