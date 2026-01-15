#include <crate/compression/diet.hh>
#include <cstring>

namespace crate {

namespace {
    constexpr size_t DLZ_HEADER_SIZE = 17;
}

struct diet_decompressor::impl {
    // Input stream
    const byte* input_ptr = nullptr;
    const byte* input_end = nullptr;

    // Output buffer
    byte* output_start = nullptr;
    byte* output_ptr = nullptr;
    byte* output_end = nullptr;

    // Bit buffer: 16-bit code word, read LSB-first
    u16 code_word = 0;
    u8 bits_remaining = 1;  // Start at 1 to trigger initial load

    void reset_state() {
        input_ptr = nullptr;
        input_end = nullptr;
        output_start = nullptr;
        output_ptr = nullptr;
        output_end = nullptr;
        code_word = 0;
        bits_remaining = 1;
    }

    u8 read_byte() {
        if (input_ptr >= input_end) {
            return 0;
        }
        return *input_ptr++;
    }

    void write_byte(u8 b) {
        if (output_ptr < output_end) {
            *output_ptr++ = b;
        }
    }

    // Get next control bit from the code word (LSB first).
    // Reloads a new 16-bit word when exhausted.
    bool next_bit() {
        bool bit = (code_word & 1) != 0;
        code_word >>= 1;
        bits_remaining--;

        if (bits_remaining == 0) {
            u8 lo = read_byte();
            u8 hi = read_byte();
            code_word = static_cast<u16>(static_cast<u16>(lo) | (static_cast<u16>(hi) << 8));
            bits_remaining = 16;
        }

        return bit;
    }

    // Rotate left through carry - emulates x86 RCL instruction
    static void rotate_left_carry(u8& value, bool carry_in) {
        value = static_cast<u8>((value << 1) | (carry_in ? 1 : 0));
    }

    // Copy match from history buffer
    result_t<void> copy_match(i16 offset, u16 length) {
        for (u16 i = 0; i < length; i++) {
            size_t current_pos = static_cast<size_t>(output_ptr - output_start);
            i32 signed_offset = static_cast<i32>(offset);

            if (signed_offset >= 0) {
                return std::unexpected(error{error_code::CorruptData,
                    "DLZ: invalid positive offset"});
            }

            size_t src_pos = static_cast<size_t>(static_cast<i64>(current_pos) + signed_offset);
            if (src_pos >= current_pos) {
                return std::unexpected(error{error_code::CorruptData,
                    "DLZ: offset underflow"});
            }

            write_byte(output_start[src_pos]);
        }
        return {};
    }

    // Parse DLZ header
    static result_t<std::pair<size_t, size_t>> parse_header(byte_span data) {
        if (data.size() < DLZ_HEADER_SIZE) {
            return std::unexpected(error{error_code::TruncatedArchive,
                "DLZ header too short"});
        }

        // Verify "dlz" signature at offset 6
        if (data[6] != 'd' || data[7] != 'l' || data[8] != 'z') {
            return std::unexpected(error{error_code::InvalidSignature,
                "Not a valid DLZ/DIET file"});
        }

        // Compressed size: 4 bits from flags + 16 bits little-endian
        u8 flags = data[9];
        u16 size_lo = static_cast<u16>(static_cast<u16>(data[10]) | (static_cast<u16>(data[11]) << 8));
        size_t compressed_size = (static_cast<size_t>(flags & 0x0F) << 16) | size_lo;

        // Decompressed size: 6 bits + 16 bits little-endian
        u8 size_hi = data[14];
        u16 decomp_lo = static_cast<u16>(static_cast<u16>(data[15]) | (static_cast<u16>(data[16]) << 8));
        size_t decompressed_size = (static_cast<size_t>((size_hi >> 2) & 0x3F) << 16) | decomp_lo;

        return std::make_pair(compressed_size, decompressed_size);
    }

    // Main decompression - faithful to original x86 algorithm.
    // Uses labeled gotos because the control flow is inherently non-structured.
    result_t<size_t> decompress_data() {
        u8 offset_lo;       // Low byte of match offset
        u8 offset_hi;       // High byte of match offset
        u16 match_length;   // Number of bytes to copy
        u8 adjustment;      // Offset/length adjustment value
        u16 counter;        // Loop counter
        bool bit;

        // Prime the bit buffer
        next_bit();

    copy_literals:
        // Copy literal bytes while control bit is 1
        while (next_bit()) {
            write_byte(read_byte());
        }

        // Control bit was 0 - decode match
        bit = next_bit();

        // Base offset: low byte from input, high byte = 0xFF (range -256 to -1)
        offset_lo = read_byte();
        offset_hi = 0xFF;

        if (bit) {
            // Extended offset path
            bit = next_bit();
            rotate_left_carry(offset_hi, bit);

            bit = next_bit();
            if (!bit) {
                // Further extend offset with variable-length encoding
                adjustment = 2;
                counter = 3;
                while (counter > 0) {
                    bit = next_bit();
                    if (bit) {
                        break;
                    }
                    bit = next_bit();
                    rotate_left_carry(offset_hi, bit);
                    adjustment <<= 1;
                    counter--;
                }
                offset_hi -= adjustment;
            }

            // Decode match length (unary + extensions)
            adjustment = 2;
            counter = 4;

        decode_length:
            while (true) {
                adjustment++;
                bit = next_bit();
                if (!bit) {
                    counter--;
                    if (counter != 0) {
                        goto decode_length;
                    }
                    // Counter exhausted - check for extended length encoding
                    bit = next_bit();
                    if (!bit) {
                        bit = next_bit();
                        if (bit) {
                            // Byte-encoded length
                            match_length = static_cast<u16>(read_byte()) + 17;
                        } else {
                            // 3-bit encoded length
                            adjustment = 0;
                            for (int i = 0; i < 3; i++) {
                                bit = next_bit();
                                rotate_left_carry(adjustment, bit);
                            }
                            match_length = adjustment + 9;
                        }
                        goto do_copy;
                    }
                    adjustment++;
                    bit = next_bit();
                    if (bit) {
                        adjustment++;
                    }
                }
                match_length = adjustment;
                break;
            }
            goto do_copy;
        }

        // Short offset path (bit was 0)
        bit = next_bit();
        if (bit) {
            // 3-bit offset extension
            for (int i = 0; i < 3; i++) {
                bit = next_bit();
                rotate_left_carry(offset_hi, bit);
            }
            offset_hi--;
            match_length = 2;
        } else {
            // Check for end-of-stream or minimal match
            if (offset_lo == offset_hi) {
                // Potential termination marker
                bit = next_bit();
                if (!bit) {
                    // End of decompression
                    goto done;
                }
                // Not end - continue copying literals
                goto copy_literals;
            }
            match_length = 2;
        }

    do_copy:
        {
            i16 offset = static_cast<i16>((static_cast<u16>(offset_hi) << 8) | offset_lo);
            auto result = copy_match(offset, match_length);
            if (!result) {
                return std::unexpected(result.error());
            }
        }
        goto copy_literals;

    done:
        return static_cast<size_t>(output_ptr - output_start);
    }
};

diet_decompressor::diet_decompressor()
    : pimpl_(std::make_unique<impl>()) {
    pimpl_->reset_state();
}

diet_decompressor::~diet_decompressor() = default;

result_t<stream_result> diet_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    // DIET decompression requires all input data at once
    if (!input_finished) {
        return stream_result::need_input(0, 0);
    }

    if (input.size() < DLZ_HEADER_SIZE) {
        return std::unexpected(error{error_code::TruncatedArchive,
            "Input too small for DLZ header"});
    }

    auto header_result = impl::parse_header(input);
    if (!header_result) {
        return std::unexpected(header_result.error());
    }

    auto [compressed_size, decompressed_size] = *header_result;

    if (input.size() < DLZ_HEADER_SIZE + compressed_size) {
        return std::unexpected(error{error_code::TruncatedArchive,
            "Input smaller than compressed size in header"});
    }

    if (output.size() < decompressed_size) {
        return std::unexpected(error{error_code::OutputBufferOverflow,
            "Output buffer too small for decompressed data"});
    }

    // Initialize decompression state
    pimpl_->input_ptr = input.data() + DLZ_HEADER_SIZE;
    pimpl_->input_end = pimpl_->input_ptr + compressed_size;
    pimpl_->output_start = output.data();
    pimpl_->output_ptr = output.data();
    pimpl_->output_end = output.data() + output.size();
    pimpl_->code_word = 0;
    pimpl_->bits_remaining = 1;

    auto result = pimpl_->decompress_data();
    if (!result) {
        return std::unexpected(result.error());
    }

    report_progress(*result, decompressed_size);

    return stream_result::done(input.size(), *result);
}

void diet_decompressor::reset() {
    pimpl_->reset_state();
}

} // namespace crate
