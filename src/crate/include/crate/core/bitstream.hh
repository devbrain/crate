#pragma once

#include <crate/core/types.hh>
#include <crate/core/error.hh>
#include <bit>

namespace crate {
    // Bit order enumeration
    enum class bit_order { LSB, MSB };

    // Bitstream reader template
    // - LSB: bits are consumed from least significant bit first (MSZIP, Deflate)
    // - MSB: bits are consumed from most significant bit first (LZX, Quantum)
    template<bit_order Order = bit_order::LSB>
    class CRATE_EXPORT bitstream_reader {
        public:
            explicit bitstream_reader(byte_span data)
                : data_(data), pos_(0), bit_buffer_(0), bits_left_(0) {
            }

            // Read n bits (1-32)
            result_t <u32> read_bits(unsigned n) {
                if (n == 0) [[unlikely]] return 0u;
                if (n > 32) [[unlikely]] {
                    return crate::make_unexpected(error{
                        error_code::InvalidBlockType,
                        "Cannot read more than 32 bits at once"
                    });
                }

                // Ensure we have enough bits
                while (bits_left_ < n) {
                    if (pos_ >= data_.size()) [[unlikely]] {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }

                    if constexpr (Order == bit_order::LSB) {
                        bit_buffer_ |= static_cast <u64>(data_[pos_++]) << bits_left_;
                    } else {
                        bit_buffer_ = (bit_buffer_ << 8) | data_[pos_++];
                    }
                    bits_left_ += 8;
                }

                u32 result;
                if constexpr (Order == bit_order::LSB) {
                    result = static_cast <u32>(bit_buffer_ & ((1ULL << n) - 1));
                    bit_buffer_ >>= n;
                    bits_left_ -= n;
                } else {
                    bits_left_ -= n;
                    result = static_cast <u32>((bit_buffer_ >> bits_left_) & ((1ULL << n) - 1));
                }

                return result;
            }

            // Read a single bit
            result_t <bool> read_bit() {
                auto r = read_bits(1);
                if (!r) return crate::make_unexpected(r.error());
                return *r != 0;
            }

            // Peek n bits without consuming
            result_t <u32> peek_bits(unsigned n) {
                if (n == 0) [[unlikely]] return 0u;

                while (bits_left_ < n) {
                    if (pos_ >= data_.size()) [[unlikely]] {
                        return crate::make_unexpected(error{error_code::InputBufferUnderflow});
                    }

                    if constexpr (Order == bit_order::LSB) {
                        bit_buffer_ |= static_cast <u64>(data_[pos_++]) << bits_left_;
                    } else {
                        bit_buffer_ = (bit_buffer_ << 8) | data_[pos_++];
                    }
                    bits_left_ += 8;
                }

                if constexpr (Order == bit_order::LSB) {
                    return static_cast <u32>(bit_buffer_ & ((1ULL << n) - 1));
                } else {
                    return static_cast <u32>((bit_buffer_ >> (bits_left_ - n)) & ((1ULL << n) - 1));
                }
            }

            // Remove n bits (after peeking)
            void remove_bits(unsigned n) {
                if constexpr (Order == bit_order::LSB) {
                    bit_buffer_ >>= n;
                }
                bits_left_ -= n;
            }

            // Read a byte (aligned or unaligned)
            result_t <u8> read_byte() {
                auto r = read_bits(8);
                if (!r) return crate::make_unexpected(r.error());
                return static_cast <u8>(*r);
            }

            // Read u16 little-endian
            result_t <u16> read_u16_le() {
                auto lo = read_byte();
                if (!lo) return crate::make_unexpected(lo.error());
                auto hi = read_byte();
                if (!hi) return crate::make_unexpected(hi.error());
                return static_cast <u16>(*lo | (*hi << 8));
            }

            // Read u32 little-endian
            result_t <u32> read_u32_le() {
                auto lo = read_u16_le();
                if (!lo) return crate::make_unexpected(lo.error());
                auto hi = read_u16_le();
                if (!hi) return crate::make_unexpected(hi.error());
                return static_cast <u32>(*lo | (static_cast <u32>(*hi) << 16));
            }

            // Align to byte boundary
            void align_to_byte() {
                if constexpr (Order == bit_order::LSB) {
                    unsigned discard = bits_left_ % 8;
                    if (discard > 0) {
                        bit_buffer_ >>= discard;
                        bits_left_ -= discard;
                    }
                } else {
                    bits_left_ -= bits_left_ % 8;
                }
            }

            // Check if at end
            [[nodiscard]] bool at_end() const {
                return pos_ >= data_.size() && bits_left_ == 0;
            }

            // Get remaining bytes (bytes not yet consumed, including buffered bits)
            [[nodiscard]] size_t remaining_bytes() const {
                // Bytes still in the buffer (already fetched but not consumed)
                size_t buffered_bytes = bits_left_ / 8;
                // Bytes not yet fetched from the data
                size_t unfetched_bytes = (pos_ <= data_.size()) ? (data_.size() - pos_) : 0;
                return buffered_bytes + unfetched_bytes;
            }

            // Get remaining bits (total unprocessed bits)
            [[nodiscard]] size_t remaining_bits() const {
                size_t unfetched_bytes = (pos_ <= data_.size()) ? (data_.size() - pos_) : 0;
                return bits_left_ + (unfetched_bytes * 8);
            }

            // Get current byte position
            [[nodiscard]] size_t byte_position() const { return pos_; }

        private:
            byte_span data_;
            size_t pos_ = 0;
            u64 bit_buffer_ = 0;
            unsigned bits_left_ = 0;
    };

    // Type aliases
    using lsb_bitstream = bitstream_reader <bit_order::LSB>;
    using msb_bitstream = bitstream_reader <bit_order::MSB>;
} // namespace crate
