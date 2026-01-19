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
        enum class state {
            READ_HEADER,
            READ_TOKEN_FLAG,
            READ_LENGTH_CODE,
            READ_LENGTH_EXTRA,
            READ_LITERAL_BINARY,
            READ_LITERAL_ASCII_START,
            READ_LITERAL_ASCII_4,
            READ_LITERAL_ASCII_6,
            READ_LITERAL_ASCII_FINAL,
            READ_DISTANCE,
            WRITE_LITERAL,
            COPY_MATCH,
            DONE
        };

        state state_ = state::READ_HEADER;

        std::array <u8, 3> header_{};
        size_t header_pos_ = 0;

        // Bit buffer (16-bit, LSB first)
        u32 bit_buff_ = 0;
        unsigned int extra_bits_ = 0;

        // Compression parameters
        int ctype_ = 0; // CMP_BINARY or CMP_ASCII
        unsigned int dsize_bits_ = 0; // Dictionary size bits (4, 5, or 6)
        u32 dsize_mask_ = 0; // Dictionary size mask

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

        // Decode state
        unsigned int length_code_ = 0;
        unsigned int ascii_value_ = 0;
        u8 pending_literal_ = 0;

        size_t match_remaining_ = 0;
        size_t match_src_pos_ = 0;

        size_t total_output_ = 0;
        size_t last_progress_report_ = 0;
        static constexpr size_t PROGRESS_INTERVAL = 4096; // Report every 4KB

        void reset_state() {
            state_ = state::READ_HEADER;
            header_.fill(0);
            header_pos_ = 0;
            bit_buff_ = 0;
            extra_bits_ = 0;
            ctype_ = 0;
            dsize_bits_ = 0;
            dsize_mask_ = 0;
            out_pos = 0x1000; // Start in middle of buffer
            out_buff.fill(0);
            length_code_ = 0;
            ascii_value_ = 0;
            pending_literal_ = 0;
            match_remaining_ = 0;
            match_src_pos_ = 0;
            total_output_ = 0;
            last_progress_report_ = 0;
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

        void init_tables() {
            if (ctype_ == CMP_ASCII) {
                std::copy(ChBitsAsc.begin(), ChBitsAsc.end(), ChBitsAscWork.begin());
                gen_asc_tabs();
            }

            gen_decode_tabs(LengthCodes.data(), LenCode.data(), LenBits.data(), LENS_SIZES);
            gen_decode_tabs(DistPosCodes.data(), DistCode.data(), DistBits.data(), DIST_SIZES);
        }

        bool try_waste_bits(unsigned int nBits, const byte*& in_ptr, const byte* in_end) {
            if (nBits == 0) {
                return true;
            }

            if (nBits <= extra_bits_) {
                extra_bits_ -= nBits;
                bit_buff_ >>= nBits;
                return true;
            }

            if (in_ptr >= in_end) {
                return false;
            }

            bit_buff_ >>= extra_bits_;
            bit_buff_ |= static_cast <u32>(*in_ptr++) << 8;
            bit_buff_ >>= (nBits - extra_bits_);
            extra_bits_ = extra_bits_ + 8 - nBits;
            return true;
        }

        void write_output(byte value, byte*& out_ptr,
                          const std::function <void(size_t, size_t)>& progress_cb) {
            out_buff[out_pos++] = value;
            *out_ptr++ = value;
            total_output_++;

            if (progress_cb && (total_output_ - last_progress_report_) >= PROGRESS_INTERVAL) {
                progress_cb(total_output_, 0);
                last_progress_report_ = total_output_;
            }

            if (out_pos >= 0x2000) {
                std::memmove(out_buff.data(), out_buff.data() + 0x1000, out_pos - 0x1000);
                out_pos -= 0x1000;
                if (match_remaining_ > 0 && match_src_pos_ >= 0x1000) {
                    match_src_pos_ -= 0x1000;
                }
            }
        }

        result_t <stream_result> decompress(
            byte_span input,
            mutable_byte_span output,
            bool input_finished,
            const std::function <void(size_t, size_t)>& progress_cb
        ) {
            const byte* in_ptr = input.data();
            const byte* in_end = input.data() + input.size();
            byte* out_ptr = output.data();
            byte* out_end = output.data() + output.size();

            while (state_ != state::DONE) {
                switch (state_) {
                    case state::READ_HEADER:
                        while (header_pos_ < header_.size()) {
                            if (in_ptr >= in_end) {
                                if (input_finished) {
                                    return std::unexpected(error{
                                        error_code::InputBufferUnderflow,
                                        "PKWARE: input too short"
                                    });
                                }
                                goto need_input;
                            }
                            header_[header_pos_++] = static_cast <u8>(*in_ptr++);
                        }

                        ctype_ = header_[0];
                        dsize_bits_ = header_[1];
                        bit_buff_ = header_[2];
                        extra_bits_ = 0;

                        if (dsize_bits_ < 4 || dsize_bits_ > 6) {
                            return std::unexpected(error{
                                error_code::CorruptData,
                                "PKWARE: invalid dictionary size"
                            });
                        }

                        if (ctype_ != CMP_BINARY && ctype_ != CMP_ASCII) {
                            return std::unexpected(error{
                                error_code::CorruptData,
                                "PKWARE: invalid compression type"
                            });
                        }

                        dsize_mask_ = 0xFFFFu >> (16 - dsize_bits_);
                        init_tables();
                        out_pos = 0x1000;
                        state_ = state::READ_TOKEN_FLAG;
                        break;

                    case state::READ_TOKEN_FLAG: {
                        bool is_length = (bit_buff_ & 1u) != 0;
                        if (!try_waste_bits(1, in_ptr, in_end)) {
                            goto need_input;
                        }
                        if (is_length) {
                            state_ = state::READ_LENGTH_CODE;
                        } else if (ctype_ == CMP_BINARY) {
                            state_ = state::READ_LITERAL_BINARY;
                        } else {
                            state_ = state::READ_LITERAL_ASCII_START;
                        }
                        break;
                    }

                    case state::READ_LENGTH_CODE:
                        length_code_ = LengthCodes[bit_buff_ & 0xFF];
                        if (!try_waste_bits(LenBits[length_code_], in_ptr, in_end)) {
                            goto need_input;
                        }
                        if (ExLenBits[length_code_] == 0) {
                            unsigned int literal = LenBase[length_code_] + 0x100;
                            if (literal >= 0x305) {
                                state_ = state::DONE;
                                break;
                            }
                            match_remaining_ = literal - 0xFE;
                            state_ = state::READ_DISTANCE;
                        } else {
                            state_ = state::READ_LENGTH_EXTRA;
                        }
                        break;

                    case state::READ_LENGTH_EXTRA: {
                        unsigned int extra_len_bits = ExLenBits[length_code_];
                        unsigned int extra_length = bit_buff_ & ((1u << extra_len_bits) - 1);
                        if (!try_waste_bits(extra_len_bits, in_ptr, in_end)) {
                            goto need_input;
                        }
                        unsigned int literal = LenBase[length_code_] + extra_length + 0x100;
                        if (literal >= 0x305) {
                            state_ = state::DONE;
                            break;
                        }
                        match_remaining_ = literal - 0xFE;
                        state_ = state::READ_DISTANCE;
                        break;
                    }

                    case state::READ_LITERAL_BINARY:
                        pending_literal_ = static_cast <u8>(bit_buff_ & 0xFF);
                        if (!try_waste_bits(8, in_ptr, in_end)) {
                            goto need_input;
                        }
                        state_ = state::WRITE_LITERAL;
                        break;

                    case state::READ_LITERAL_ASCII_START:
                        if (bit_buff_ & 0xFF) {
                            ascii_value_ = offs2C34[bit_buff_ & 0xFF];
                            if (ascii_value_ == 0xFF) {
                                if (bit_buff_ & 0x3F) {
                                    state_ = state::READ_LITERAL_ASCII_4;
                                } else {
                                    state_ = state::READ_LITERAL_ASCII_6;
                                }
                            } else {
                                state_ = state::READ_LITERAL_ASCII_FINAL;
                            }
                        } else {
                            if (!try_waste_bits(8, in_ptr, in_end)) {
                                goto need_input;
                            }
                            ascii_value_ = offs2EB4[bit_buff_ & 0xFF];
                            state_ = state::READ_LITERAL_ASCII_FINAL;
                        }
                        break;

                    case state::READ_LITERAL_ASCII_4:
                        if (!try_waste_bits(4, in_ptr, in_end)) {
                            goto need_input;
                        }
                        ascii_value_ = offs2D34[bit_buff_ & 0xFF];
                        state_ = state::READ_LITERAL_ASCII_FINAL;
                        break;

                    case state::READ_LITERAL_ASCII_6:
                        if (!try_waste_bits(6, in_ptr, in_end)) {
                            goto need_input;
                        }
                        ascii_value_ = offs2E34[bit_buff_ & 0x7F];
                        state_ = state::READ_LITERAL_ASCII_FINAL;
                        break;

                    case state::READ_LITERAL_ASCII_FINAL:
                        if (!try_waste_bits(ChBitsAscWork[ascii_value_], in_ptr, in_end)) {
                            goto need_input;
                        }
                        pending_literal_ = static_cast <u8>(ascii_value_);
                        state_ = state::WRITE_LITERAL;
                        break;

                    case state::READ_DISTANCE: {
                        unsigned int dist_pos_code = DistPosCodes[bit_buff_ & 0xFF];
                        if (!try_waste_bits(DistBits[dist_pos_code], in_ptr, in_end)) {
                            goto need_input;
                        }

                        unsigned int distance;
                        if (match_remaining_ == 2) {
                            distance = (dist_pos_code << 2) | (bit_buff_ & 0x03);
                            if (!try_waste_bits(2, in_ptr, in_end)) {
                                goto need_input;
                            }
                        } else {
                            distance = (dist_pos_code << dsize_bits_) | (bit_buff_ & dsize_mask_);
                            if (!try_waste_bits(dsize_bits_, in_ptr, in_end)) {
                                goto need_input;
                            }
                        }

                        match_src_pos_ = out_pos - (distance + 1);
                        state_ = state::COPY_MATCH;
                        break;
                    }

                    case state::WRITE_LITERAL:
                        if (out_ptr >= out_end) {
                            goto need_output;
                        }
                        write_output(static_cast <byte>(pending_literal_), out_ptr, progress_cb);
                        state_ = state::READ_TOKEN_FLAG;
                        break;

                    case state::COPY_MATCH:
                        while (match_remaining_ > 0) {
                            if (out_ptr >= out_end) {
                                goto need_output;
                            }
                            byte value = out_buff[match_src_pos_++];
                            write_output(value, out_ptr, progress_cb);
                            match_remaining_--;
                        }
                        state_ = state::READ_TOKEN_FLAG;
                        break;

                    case state::DONE:
                        break;
                }
            }

            if (progress_cb) {
                progress_cb(total_output_, 0);
            }
            return stream_result::done(
                static_cast <size_t>(in_ptr - input.data()),
                static_cast <size_t>(out_ptr - output.data())
            );

        need_output:
            return stream_result::need_output(
                static_cast <size_t>(in_ptr - input.data()),
                static_cast <size_t>(out_ptr - output.data())
            );

        need_input:
            if (input_finished) {
                state_ = state::DONE;
                if (progress_cb) {
                    progress_cb(total_output_, 0);
                }
                return stream_result::done(
                    static_cast <size_t>(in_ptr - input.data()),
                    static_cast <size_t>(out_ptr - output.data())
                );
            }
            return stream_result::need_input(
                static_cast <size_t>(in_ptr - input.data()),
                static_cast <size_t>(out_ptr - output.data())
            );
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
        return pimpl_->decompress(
            input,
            output,
            input_finished,
            [this](size_t written, size_t total) {
                report_progress(written, total);
            }
        );
    }

    void explode_decompressor::reset() {
        pimpl_->reset_state();
    }
} // namespace crate
