#include <crate/compression/diet.hh>
#include <array>
#include <cstring>

namespace crate {

namespace {
    // DIET uses an 8KB ring buffer for LZ77 back-references
    constexpr size_t RING_BUFFER_SIZE = 8192;
    constexpr size_t RING_BUFFER_MASK = RING_BUFFER_SIZE - 1;
}

struct diet_decompressor::impl {
    // Ring buffer for LZ77 decompression
    std::array<byte, RING_BUFFER_SIZE> ring_buffer{};
    size_t ring_pos = 0;

    // Bit buffer for reading compressed data (LSB-first)
    u32 bit_buffer = 0;
    int bits_left = 0;

    // Input stream tracking
    const byte* input_ptr = nullptr;
    const byte* input_end = nullptr;

    // Output tracking
    byte* output_ptr = nullptr;
    byte* output_end = nullptr;
    size_t bytes_written = 0;

    void reset_state() {
        ring_buffer.fill(0x20);  // Fill with spaces (common for text)
        ring_pos = RING_BUFFER_SIZE - 256;  // Start position
        bit_buffer = 0;
        bits_left = 0;
        input_ptr = nullptr;
        input_end = nullptr;
        output_ptr = nullptr;
        output_end = nullptr;
        bytes_written = 0;
    }

    bool has_input_bytes(size_t n) const {
        return static_cast<size_t>(input_end - input_ptr) >= n;
    }

    bool has_output_space(size_t n) const {
        return static_cast<size_t>(output_end - output_ptr) >= n;
    }

    // Read a byte from input
    u8 read_byte() {
        if (input_ptr >= input_end) {
            return 0;
        }
        return *input_ptr++;
    }

    // Fill bit buffer with more bits (16 bits at a time, LSB first)
    void fill_bits() {
        while (bits_left <= 16 && has_input_bytes(1)) {
            bit_buffer |= static_cast<u32>(read_byte()) << bits_left;
            bits_left += 8;
        }
    }

    // Get a single bit (LSB first)
    int get_bit() {
        if (bits_left < 1) {
            fill_bits();
            if (bits_left < 1) {
                return -1;  // No more bits
            }
        }
        int bit = bit_buffer & 1;
        bit_buffer >>= 1;
        bits_left--;
        return bit;
    }

    // Get multiple bits (LSB first)
    int get_bits(int count) {
        if (count <= 0) return 0;

        while (bits_left < count) {
            fill_bits();
            if (bits_left < count) {
                return -1;  // Not enough bits
            }
        }

        int value = static_cast<int>(bit_buffer & ((1u << count) - 1));
        bit_buffer >>= count;
        bits_left -= count;
        return value;
    }

    // Write a byte to output and ring buffer
    void write_byte(byte b) {
        if (output_ptr < output_end) {
            *output_ptr++ = b;
            bytes_written++;
        }
        ring_buffer[ring_pos] = b;
        ring_pos = (ring_pos + 1) & RING_BUFFER_MASK;
    }

    // Copy from ring buffer (LZ77 match)
    void copy_from_ring(size_t offset, size_t length) {
        size_t src_pos = (ring_pos - offset) & RING_BUFFER_MASK;
        for (size_t i = 0; i < length && output_ptr < output_end; i++) {
            byte b = ring_buffer[src_pos];
            write_byte(b);
            src_pos = (src_pos + 1) & RING_BUFFER_MASK;
        }
    }

    // Read match length using DIET's variable-length encoding
    // Returns -1 on error, 0 for stop code
    int read_match_length() {
        int bit = get_bit();
        if (bit < 0) return -1;

        if (bit == 1) {
            // Length 2
            return 2;
        }

        bit = get_bit();
        if (bit < 0) return -1;

        if (bit == 1) {
            // Length 3
            return 3;
        }

        bit = get_bit();
        if (bit < 0) return -1;

        if (bit == 1) {
            // Length 4
            return 4;
        }

        bit = get_bit();
        if (bit < 0) return -1;

        if (bit == 1) {
            // Length 5-6 (1 more bit)
            int extra = get_bit();
            if (extra < 0) return -1;
            return 5 + extra;
        }

        // 0000 prefix
        bit = get_bit();
        if (bit < 0) return -1;

        if (bit == 1) {
            // Length 7-8 (1 more bit)
            int extra = get_bit();
            if (extra < 0) return -1;
            return 7 + extra;
        }

        // 00000 prefix - read 3 more bits for length 9-16
        int extra = get_bits(3);
        if (extra < 0) return -1;

        if (extra != 0) {
            return 9 + extra - 1;  // 9-16
        }

        // 00000 000 - read byte for extended length or stop code
        if (!has_input_bytes(1)) {
            return -1;
        }

        int len_byte = read_byte();
        if (len_byte == 0) {
            return 0;  // Stop code
        }

        return 16 + len_byte;  // 17-272
    }

    // Read match offset for 2-byte match
    int read_offset_2byte() {
        int bit = get_bit();
        if (bit < 0) return -1;

        if (bit == 1) {
            // Short offset: read 8 bits
            int offset = get_bits(8);
            if (offset < 0) return -1;
            return offset + 1;  // 1-256
        }

        // Longer offset
        bit = get_bit();
        if (bit < 0) return -1;

        if (bit == 1) {
            // Medium offset: read 9 bits
            int offset = get_bits(9);
            if (offset < 0) return -1;
            return 256 + offset + 1;  // 257-768
        }

        // Long offset: read 10 bits
        int offset = get_bits(10);
        if (offset < 0) return -1;
        return 768 + offset + 1;  // 769-1792
    }

    // Read match offset for longer matches
    int read_offset_long([[maybe_unused]] int length) {
        int bit = get_bit();
        if (bit < 0) return -1;

        if (bit == 1) {
            // Short offset: read 8 bits
            int offset = get_bits(8);
            if (offset < 0) return -1;
            return offset + 1;  // 1-256
        }

        bit = get_bit();
        if (bit < 0) return -1;

        if (bit == 1) {
            // Medium-short: read 9 bits
            int offset = get_bits(9);
            if (offset < 0) return -1;
            return 256 + offset + 1;  // 257-768
        }

        bit = get_bit();
        if (bit < 0) return -1;

        if (bit == 1) {
            // Medium: read 10 bits
            int offset = get_bits(10);
            if (offset < 0) return -1;
            return 768 + offset + 1;  // 769-1792
        }

        bit = get_bit();
        if (bit < 0) return -1;

        if (bit == 1) {
            // Medium-long: read 11 bits
            int offset = get_bits(11);
            if (offset < 0) return -1;
            return 1792 + offset + 1;  // 1793-3840
        }

        bit = get_bit();
        if (bit < 0) return -1;

        if (bit == 1) {
            // Long: read 12 bits
            int offset = get_bits(12);
            if (offset < 0) return -1;
            return 3840 + offset + 1;  // 3841-8192
        }

        // Maximum: read 13 bits
        int offset = get_bits(13);
        if (offset < 0) return -1;
        return offset + 1;
    }

    // Main decompression routine
    result_t<size_t> decompress_data() {
        while (true) {
            int bit = get_bit();
            if (bit < 0) {
                // End of input
                break;
            }

            if (bit == 1) {
                // Literal byte
                if (!has_input_bytes(1)) {
                    return std::unexpected(error{error_code::InputBufferUnderflow,
                        "DIET: unexpected end of input"});
                }
                write_byte(read_byte());
                continue;
            }

            // Match or special code
            bit = get_bit();
            if (bit < 0) {
                return std::unexpected(error{error_code::InputBufferUnderflow,
                    "DIET: unexpected end of input"});
            }

            if (bit == 0) {
                // Could be stop code or 2-byte match
                int length = read_match_length();
                if (length < 0) {
                    return std::unexpected(error{error_code::CorruptData,
                        "DIET: invalid match length"});
                }
                if (length == 0) {
                    // Stop code
                    break;
                }

                int offset;
                if (length == 2) {
                    offset = read_offset_2byte();
                } else {
                    offset = read_offset_long(length);
                }

                if (offset < 0) {
                    return std::unexpected(error{error_code::CorruptData,
                        "DIET: invalid match offset"});
                }

                if (!has_output_space(static_cast<size_t>(length))) {
                    return std::unexpected(error{error_code::OutputBufferOverflow,
                        "DIET: output buffer too small"});
                }

                copy_from_ring(static_cast<size_t>(offset), static_cast<size_t>(length));
            } else {
                // 01 prefix - multi-byte match with different encoding
                int length = read_match_length();
                if (length < 0) {
                    return std::unexpected(error{error_code::CorruptData,
                        "DIET: invalid match length"});
                }
                if (length == 0) {
                    break;
                }

                int offset = read_offset_long(length);
                if (offset < 0) {
                    return std::unexpected(error{error_code::CorruptData,
                        "DIET: invalid match offset"});
                }

                if (!has_output_space(static_cast<size_t>(length))) {
                    return std::unexpected(error{error_code::OutputBufferOverflow,
                        "DIET: output buffer too small"});
                }

                copy_from_ring(static_cast<size_t>(offset), static_cast<size_t>(length));
            }
        }

        return bytes_written;
    }
};

diet_decompressor::diet_decompressor()
    : pimpl_(std::make_unique<impl>()) {
    pimpl_->reset_state();
}

diet_decompressor::~diet_decompressor() = default;

result_t<size_t> diet_decompressor::decompress(byte_span input, mutable_byte_span output) {
    pimpl_->input_ptr = input.data();
    pimpl_->input_end = input.data() + input.size();
    pimpl_->output_ptr = output.data();
    pimpl_->output_end = output.data() + output.size();
    pimpl_->bytes_written = 0;

    return pimpl_->decompress_data();
}

void diet_decompressor::reset() {
    pimpl_->reset_state();
}

} // namespace crate
