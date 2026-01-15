// PKWARE DCL Explode decompressor
// Based on pklib by Ladislav Zezula (MIT License)
// Original: https://codeberg.org/implode-compression-impls/pklib

#include <crate/compression/explode.hh>
#include <array>
#include <cstring>

namespace crate {
    namespace {
        // Compression types
        constexpr int CMP_BINARY = 0;
        constexpr int CMP_ASCII = 1;

        // Buffer sizes
        constexpr size_t IN_BUFF_SIZE = 0x800;
        constexpr size_t OUT_BUFF_SIZE = 0x2204;

        // Lookup table sizes
        constexpr size_t DIST_SIZES = 0x40;
        constexpr size_t CH_BITS_ASC_SIZE = 0x100;
        constexpr size_t LENS_SIZES = 0x10;

        // Distance position bit counts
        constexpr std::array <u8, DIST_SIZES> DistBits = {
            0x02, 0x04, 0x04, 0x05, 0x05, 0x05, 0x05, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
            0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
            0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
            0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08
        };

        // Distance position codes
        constexpr std::array <u8, DIST_SIZES> DistCode = {
            0x03, 0x0D, 0x05, 0x19, 0x09, 0x11, 0x01, 0x3E, 0x1E, 0x2E, 0x0E, 0x36, 0x16, 0x26, 0x06, 0x3A,
            0x1A, 0x2A, 0x0A, 0x32, 0x12, 0x22, 0x42, 0x02, 0x7C, 0x3C, 0x5C, 0x1C, 0x6C, 0x2C, 0x4C, 0x0C,
            0x74, 0x34, 0x54, 0x14, 0x64, 0x24, 0x44, 0x04, 0x78, 0x38, 0x58, 0x18, 0x68, 0x28, 0x48, 0x08,
            0xF0, 0x70, 0xB0, 0x30, 0xD0, 0x50, 0x90, 0x10, 0xE0, 0x60, 0xA0, 0x20, 0xC0, 0x40, 0x80, 0x00
        };

        // Extra bits for length codes
        constexpr std::array <u8, LENS_SIZES> ExLenBits = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
        };

        // Base values for length codes
        constexpr std::array <u16, LENS_SIZES> LenBase = {
            0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007,
            0x0008, 0x000A, 0x000E, 0x0016, 0x0026, 0x0046, 0x0086, 0x0106
        };

        // Bit counts for length codes
        constexpr std::array <u8, LENS_SIZES> LenBits = {
            0x03, 0x02, 0x03, 0x03, 0x04, 0x04, 0x04, 0x05, 0x05, 0x05, 0x05, 0x06, 0x06, 0x06, 0x07, 0x07
        };

        // Length code values
        constexpr std::array <u8, LENS_SIZES> LenCode = {
            0x05, 0x03, 0x01, 0x06, 0x0A, 0x02, 0x0C, 0x14, 0x04, 0x18, 0x08, 0x30, 0x10, 0x20, 0x40, 0x00
        };

        // ASCII character bit counts
        constexpr std::array <u8, CH_BITS_ASC_SIZE> ChBitsAsc = {
            0x0B, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x08, 0x07, 0x0C, 0x0C, 0x07, 0x0C, 0x0C,
            0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0D, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C,
            0x04, 0x0A, 0x08, 0x0C, 0x0A, 0x0C, 0x0A, 0x08, 0x07, 0x07, 0x08, 0x09, 0x07, 0x06, 0x07, 0x08,
            0x07, 0x06, 0x07, 0x07, 0x07, 0x07, 0x08, 0x07, 0x07, 0x08, 0x08, 0x0C, 0x0B, 0x07, 0x09, 0x0B,
            0x0C, 0x06, 0x07, 0x06, 0x06, 0x05, 0x07, 0x08, 0x08, 0x06, 0x0B, 0x09, 0x06, 0x07, 0x06, 0x06,
            0x07, 0x0B, 0x06, 0x06, 0x06, 0x07, 0x09, 0x08, 0x09, 0x09, 0x0B, 0x08, 0x0B, 0x09, 0x0C, 0x08,
            0x0C, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x0B, 0x07, 0x05, 0x06, 0x05, 0x05,
            0x06, 0x0A, 0x05, 0x05, 0x05, 0x05, 0x08, 0x07, 0x08, 0x08, 0x0A, 0x0B, 0x0B, 0x0C, 0x0C, 0x0C,
            0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D,
            0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D,
            0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D,
            0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C,
            0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C,
            0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C,
            0x0D, 0x0C, 0x0D, 0x0D, 0x0D, 0x0C, 0x0D, 0x0D, 0x0D, 0x0C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0C, 0x0D,
            0x0D, 0x0D, 0x0C, 0x0C, 0x0C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D
        };

        // ASCII character codes
        constexpr std::array <u16, CH_BITS_ASC_SIZE> ChCodeAsc = {
            0x0490, 0x0FE0, 0x07E0, 0x0BE0, 0x03E0, 0x0DE0, 0x05E0, 0x09E0,
            0x01E0, 0x00B8, 0x0062, 0x0EE0, 0x06E0, 0x0022, 0x0AE0, 0x02E0,
            0x0CE0, 0x04E0, 0x08E0, 0x00E0, 0x0F60, 0x0760, 0x0B60, 0x0360,
            0x0D60, 0x0560, 0x1240, 0x0960, 0x0160, 0x0E60, 0x0660, 0x0A60,
            0x000F, 0x0250, 0x0038, 0x0260, 0x0050, 0x0C60, 0x0390, 0x00D8,
            0x0042, 0x0002, 0x0058, 0x01B0, 0x007C, 0x0029, 0x003C, 0x0098,
            0x005C, 0x0009, 0x001C, 0x006C, 0x002C, 0x004C, 0x0018, 0x000C,
            0x0074, 0x00E8, 0x0068, 0x0460, 0x0090, 0x0034, 0x00B0, 0x0710,
            0x0860, 0x0031, 0x0054, 0x0011, 0x0021, 0x0017, 0x0014, 0x00A8,
            0x0028, 0x0001, 0x0310, 0x0130, 0x003E, 0x0064, 0x001E, 0x002E,
            0x0024, 0x0510, 0x000E, 0x0036, 0x0016, 0x0044, 0x0030, 0x00C8,
            0x01D0, 0x00D0, 0x0110, 0x0048, 0x0610, 0x0150, 0x0060, 0x0088,
            0x0FA0, 0x0007, 0x0026, 0x0006, 0x003A, 0x001B, 0x001A, 0x002A,
            0x000A, 0x000B, 0x0210, 0x0004, 0x0013, 0x0032, 0x0003, 0x001D,
            0x0012, 0x0190, 0x000D, 0x0015, 0x0005, 0x0019, 0x0008, 0x0078,
            0x00F0, 0x0070, 0x0290, 0x0410, 0x0010, 0x07A0, 0x0BA0, 0x03A0,
            0x0240, 0x1C40, 0x0C40, 0x1440, 0x0440, 0x1840, 0x0840, 0x1040,
            0x0040, 0x1F80, 0x0F80, 0x1780, 0x0780, 0x1B80, 0x0B80, 0x1380,
            0x0380, 0x1D80, 0x0D80, 0x1580, 0x0580, 0x1980, 0x0980, 0x1180,
            0x0180, 0x1E80, 0x0E80, 0x1680, 0x0680, 0x1A80, 0x0A80, 0x1280,
            0x0280, 0x1C80, 0x0C80, 0x1480, 0x0480, 0x1880, 0x0880, 0x1080,
            0x0080, 0x1F00, 0x0F00, 0x1700, 0x0700, 0x1B00, 0x0B00, 0x1300,
            0x0DA0, 0x05A0, 0x09A0, 0x01A0, 0x0EA0, 0x06A0, 0x0AA0, 0x02A0,
            0x0CA0, 0x04A0, 0x08A0, 0x00A0, 0x0F20, 0x0720, 0x0B20, 0x0320,
            0x0D20, 0x0520, 0x0920, 0x0120, 0x0E20, 0x0620, 0x0A20, 0x0220,
            0x0C20, 0x0420, 0x0820, 0x0020, 0x0FC0, 0x07C0, 0x0BC0, 0x03C0,
            0x0DC0, 0x05C0, 0x09C0, 0x01C0, 0x0EC0, 0x06C0, 0x0AC0, 0x02C0,
            0x0CC0, 0x04C0, 0x08C0, 0x00C0, 0x0F40, 0x0740, 0x0B40, 0x0340,
            0x0300, 0x0D40, 0x1D00, 0x0D00, 0x1500, 0x0540, 0x0500, 0x1900,
            0x0900, 0x0940, 0x1100, 0x0100, 0x1E00, 0x0E00, 0x0140, 0x1600,
            0x0600, 0x1A00, 0x0E40, 0x0640, 0x0A40, 0x0A00, 0x1200, 0x0200,
            0x1C00, 0x0C00, 0x1400, 0x0400, 0x1800, 0x0800, 0x1000, 0x0000
        };
    } // anonymous namespace

    struct explode_decompressor::impl {
        // Input tracking
        const byte* input_ptr = nullptr;
        const byte* input_end = nullptr;

        // Output tracking
        byte* output_start = nullptr;
        byte* output_ptr = nullptr;
        byte* output_end = nullptr;
        size_t total_expected = 0;

        // Progress reporting
        std::function <void(size_t, size_t)> progress_reporter;
        size_t last_progress_report = 0;
        static constexpr size_t PROGRESS_INTERVAL = 4096; // Report every 4KB

        // Bit buffer (16-bit, LSB first)
        u32 bit_buff = 0;
        unsigned int extra_bits = 0;

        // Compression parameters
        int ctype = 0; // CMP_BINARY or CMP_ASCII
        unsigned int dsize_bits = 0; // Dictionary size bits (4, 5, or 6)
        u32 dsize_mask = 0; // Dictionary size mask

        // Decode tables (built at runtime)
        std::array <u8, 0x100> DistPosCodes{};
        std::array <u8, 0x100> LengthCodes{};
        std::array <u8, 0x100> offs2C34{};
        std::array <u8, 0x100> offs2D34{};
        std::array <u8, 0x80> offs2E34{};
        std::array <u8, 0x100> offs2EB4{};
        std::array <u8, CH_BITS_ASC_SIZE> ChBitsAscWork{};

        // Output ring buffer for back-references
        std::array <byte, OUT_BUFF_SIZE> out_buff{};
        size_t out_pos = 0;

        void reset_state() {
            input_ptr = nullptr;
            input_end = nullptr;
            output_start = nullptr;
            output_ptr = nullptr;
            output_end = nullptr;
            total_expected = 0;
            progress_reporter = nullptr;
            last_progress_report = 0;
            bit_buff = 0;
            extra_bits = 0;
            ctype = 0;
            dsize_bits = 0;
            dsize_mask = 0;
            out_pos = 0x1000; // Start in middle of buffer
            out_buff.fill(0);
        }

        bool has_input() const {
            return input_ptr < input_end;
        }

        u8 read_byte() {
            if (input_ptr >= input_end) {
                return 0;
            }
            return static_cast <u8>(*input_ptr++);
        }

        // Generate decode tables from code/bits arrays
        void gen_decode_tabs(u8* positions, const u8* start_indexes,
                             const u8* length_bits, size_t elements) {
            for (size_t i = 0; i < elements; i++) {
                unsigned int length = 1u << length_bits[i];
                for (unsigned int index = start_indexes[i]; index < 0x100; index += length) {
                    positions[index] = static_cast <u8>(i);
                }
            }
        }

        // Generate ASCII decode tables
        void gen_asc_tabs() {
            const u16* pChCodeAsc = &ChCodeAsc[0xFF];

            for (size_t count = 0x00FF; pChCodeAsc >= ChCodeAsc.data(); pChCodeAsc--, count--) {
                u8* pChBitsAsc = &ChBitsAscWork[count];
                u8 bits_asc = *pChBitsAsc;

                if (bits_asc <= 8) {
                    unsigned int add = 1u << bits_asc;
                    unsigned int acc = *pChCodeAsc;
                    do {
                        offs2C34[acc] = static_cast <u8>(count);
                        acc += add;
                    }
                    while (acc < 0x100);
                } else if (unsigned int acc = (*pChCodeAsc & 0xFF); acc != 0) {
                    offs2C34[acc] = 0xFF;

                    if (*pChCodeAsc & 0x3F) {
                        bits_asc -= 4;
                        *pChBitsAsc = bits_asc;

                        unsigned int add = 1u << bits_asc;
                        acc = *pChCodeAsc >> 4;
                        do {
                            offs2D34[acc] = static_cast <u8>(count);
                            acc += add;
                        }
                        while (acc < 0x100);
                    } else {
                        bits_asc -= 6;
                        *pChBitsAsc = bits_asc;

                        unsigned int add = 1u << bits_asc;
                        acc = *pChCodeAsc >> 6;
                        do {
                            offs2E34[acc] = static_cast <u8>(count);
                            acc += add;
                        }
                        while (acc < 0x80);
                    }
                } else {
                    bits_asc -= 8;
                    *pChBitsAsc = bits_asc;

                    unsigned int add = 1u << bits_asc;
                    acc = *pChCodeAsc >> 8;
                    do {
                        offs2EB4[acc] = static_cast <u8>(count);
                        acc += add;
                    }
                    while (acc < 0x100);
                }
            }
        }

        // Remove bits from buffer, refilling as needed
        // Returns true on success, false if no more input
        bool waste_bits(unsigned int nBits) {
            if (nBits <= extra_bits) {
                extra_bits -= nBits;
                bit_buff >>= nBits;
                return true;
            }

            bit_buff >>= extra_bits;
            if (!has_input()) {
                return false;
            }

            bit_buff |= static_cast <u32>(read_byte()) << 8;
            bit_buff >>= (nBits - extra_bits);
            extra_bits = (extra_bits - nBits) + 8;
            return true;
        }

        // Decode a literal or length code
        // Returns: 0x000-0x0FF: literal byte
        //          0x100-0x304: repetition length (subtract 0xFE for actual length)
        //          0x305: end of stream
        //          0x306: error
        unsigned int decode_lit() {
            // If low bit is set, it's a length code
            if (bit_buff & 1) {
                if (!waste_bits(1)) return 0x306;

                unsigned int length_code = LengthCodes[bit_buff & 0xFF];
                if (!waste_bits(LenBits[length_code])) return 0x306;

                // Handle extra length bits
                if (unsigned int extra_len_bits = ExLenBits[length_code]; extra_len_bits != 0) {
                    unsigned int extra_length = bit_buff & ((1u << extra_len_bits) - 1);
                    if (!waste_bits(extra_len_bits)) {
                        if ((length_code + extra_length) != 0x10E)
                            return 0x306;
                    }
                    length_code = LenBase[length_code] + extra_length;
                }

                return length_code + 0x100;
            }

            // Low bit not set - it's a literal
            if (!waste_bits(1)) return 0x306;

            // Binary mode: next 8 bits are the literal
            if (ctype == CMP_BINARY) {
                unsigned int literal = bit_buff & 0xFF;
                if (!waste_bits(8)) return 0x306;
                return literal;
            }

            // ASCII mode: use Huffman tables
            unsigned int value;
            if (bit_buff & 0xFF) {
                value = offs2C34[bit_buff & 0xFF];
                if (value == 0xFF) {
                    if (bit_buff & 0x3F) {
                        if (!waste_bits(4)) return 0x306;
                        value = offs2D34[bit_buff & 0xFF];
                    } else {
                        if (!waste_bits(6)) return 0x306;
                        value = offs2E34[bit_buff & 0x7F];
                    }
                }
            } else {
                if (!waste_bits(8)) return 0x306;
                value = offs2EB4[bit_buff & 0xFF];
            }

            return waste_bits(ChBitsAscWork[value]) ? value : 0x306;
        }

        // Decode distance for back-reference
        unsigned int decode_dist(unsigned int rep_length) {
            unsigned int dist_pos_code = DistPosCodes[bit_buff & 0xFF];
            if (!waste_bits(DistBits[dist_pos_code])) return 0;

            unsigned int distance;
            if (rep_length == 2) {
                // 2-byte match: use 2 extra bits
                distance = (dist_pos_code << 2) | (bit_buff & 0x03);
                if (!waste_bits(2)) return 0;
            } else {
                // Longer match: use dsize_bits extra bits
                distance = (dist_pos_code << dsize_bits) | (bit_buff & dsize_mask);
                if (!waste_bits(dsize_bits)) return 0;
            }

            return distance + 1;
        }

        // Write a byte to output
        void write_byte(byte b) {
            out_buff[out_pos++] = b;

            if (output_ptr < output_end) {
                *output_ptr++ = b;

                // Report progress periodically
                size_t bytes_written = static_cast <size_t>(output_ptr - output_start);
                if (progress_reporter && (bytes_written - last_progress_report) >= PROGRESS_INTERVAL) {
                    progress_reporter(bytes_written, total_expected);
                    last_progress_report = bytes_written;
                }
            }

            // Wrap ring buffer
            if (out_pos >= 0x2000) {
                std::memmove(out_buff.data(), out_buff.data() + 0x1000, out_pos - 0x1000);
                out_pos -= 0x1000;
            }
        }

        // Main decompression routine
        result_t <size_t> decompress_data() {
            // Read header
            if (input_end - input_ptr < 3) {
                return std::unexpected(error{
                    error_code::InputBufferUnderflow,
                    "PKWARE: input too short"
                });
            }

            ctype = read_byte();
            dsize_bits = read_byte();
            bit_buff = read_byte();
            extra_bits = 0;

            // Validate parameters
            if (dsize_bits < 4 || dsize_bits > 6) {
                return std::unexpected(error{
                    error_code::CorruptData,
                    "PKWARE: invalid dictionary size"
                });
            }

            if (ctype != CMP_BINARY && ctype != CMP_ASCII) {
                return std::unexpected(error{
                    error_code::CorruptData,
                    "PKWARE: invalid compression type"
                });
            }

            dsize_mask = 0xFFFFu >> (16 - dsize_bits);

            // Initialize decode tables
            if (ctype == CMP_ASCII) {
                std::copy(ChBitsAsc.begin(), ChBitsAsc.end(), ChBitsAscWork.begin());
                gen_asc_tabs();
            }

            gen_decode_tabs(LengthCodes.data(), LenCode.data(), LenBits.data(), LENS_SIZES);
            gen_decode_tabs(DistPosCodes.data(), DistCode.data(), DistBits.data(), DIST_SIZES);

            out_pos = 0x1000;

            // Main decompression loop
            while (true) {
                unsigned int literal = decode_lit();

                if (literal >= 0x305) {
                    // 0x305 = explicit end of stream
                    // 0x306 = end of input (may be implicit end)
                    break;
                }

                if (literal >= 0x100) {
                    // Back-reference
                    unsigned int rep_length = literal - 0xFE;
                    unsigned int distance = decode_dist(rep_length);

                    if (distance == 0) {
                        // End of input while reading distance - treat as end of stream
                        break;
                    }

                    // Copy from ring buffer
                    size_t src_pos = out_pos - distance;
                    for (unsigned int i = 0; i < rep_length; i++) {
                        write_byte(out_buff[src_pos + i]);
                    }
                } else {
                    // Literal byte
                    write_byte(static_cast <byte>(literal));
                }
            }

            return static_cast <size_t>(output_ptr - output_start);
        }
    };

    explode_decompressor::explode_decompressor()
        : pimpl_(std::make_unique <impl>()) {
        pimpl_->reset_state();
    }

    explode_decompressor::~explode_decompressor() = default;

    result_t<stream_result> explode_decompressor::decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished
    ) {
        // This decompressor requires all input at once
        if (!input_finished) {
            // If streaming partial data, we can't do anything yet
            // Return 0 bytes processed, waiting for more
            return stream_result::need_input(0, 0);
        }

        pimpl_->input_ptr = input.data();
        pimpl_->input_end = input.data() + input.size();
        pimpl_->output_start = output.data();
        pimpl_->output_ptr = output.data();
        pimpl_->output_end = output.data() + output.size();
        pimpl_->total_expected = output.size();
        pimpl_->last_progress_report = 0;

        // Set up progress reporter if callback is set
        if (progress_cb_) {
            pimpl_->progress_reporter = [this](size_t written, size_t total) {
                report_progress(written, total);
            };
        } else {
            pimpl_->progress_reporter = nullptr;
        }

        auto result = pimpl_->decompress_data();

        if (!result) {
            return std::unexpected(result.error());
        }

        // Report final progress
        if (progress_cb_) {
            report_progress(*result, pimpl_->total_expected);
        }

        return stream_result::done(input.size(), *result);
    }

    void explode_decompressor::reset() {
        pimpl_->reset_state();
    }
} // namespace crate
