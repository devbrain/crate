#include <crate/compression/rnc.hh>
#include <cstring>

namespace crate {

namespace rnc {

// RNC CRC-16 table (same polynomial as in reference implementation)
const std::array<u16, 256> crc_table = {{
    0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
    0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
    0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
    0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
    0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
    0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
    0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
    0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
    0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
    0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
    0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
    0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
    0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
    0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
    0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
    0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
    0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
    0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
    0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
    0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
    0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
    0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
    0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
    0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
    0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
    0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
    0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
    0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
    0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
    0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
    0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
    0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040
}};

u16 calculate_crc(byte_span data) {
    u16 crc = 0;
    for (auto b : data) {
        crc = static_cast<u16>((crc >> 8) ^ crc_table[(crc ^ static_cast<u8>(b)) & 0xFF]);
    }
    return crc;
}

}  // namespace rnc

namespace {
    constexpr size_t WINDOW_SIZE = 65536;
}

struct rnc_decompressor::impl {
    // Huffman table entry
    struct huf_entry {
        u32 code;       // Huffman code (LSB-aligned for method 1)
        u16 bit_depth;  // Number of bits in code
    };

    // State machine states
    enum class state : u8 {
        READ_HEADER,
        READ_INIT_BITS,

        // Method 1 states
        M1_READ_RAW_TABLE,
        M1_READ_LEN_TABLE,
        M1_READ_POS_TABLE,
        M1_READ_SUBCHUNK_COUNT,
        M1_DECODE_RAW_LENGTH,
        M1_COPY_LITERALS,
        M1_CHECK_SUBCHUNK,
        M1_DECODE_OFFSET,
        M1_DECODE_LENGTH,
        M1_COPY_MATCH,

        // Method 2 states
        M2_DECODE_SYMBOL,
        M2_COPY_LITERAL,
        M2_DECODE_MATCH_COUNT_1,
        M2_DECODE_MATCH_COUNT_2,
        M2_DECODE_MATCH_COUNT_3,
        M2_DECODE_MATCH_COUNT_4,
        M2_DECODE_MATCH_COUNT_5,
        M2_DECODE_MATCH_OFFSET_1,
        M2_DECODE_MATCH_OFFSET_2,
        M2_DECODE_MATCH_OFFSET_3,
        M2_DECODE_MATCH_OFFSET_4,
        M2_DECODE_MATCH_OFFSET_5,
        M2_DECODE_LONG_LITERAL_COUNT,
        M2_COPY_LONG_LITERALS,
        M2_COPY_MATCH,
        M2_CHECK_END,

        DONE
    };

    state state_ = state::READ_HEADER;

    // Header data
    std::array<u8, rnc::HEADER_SIZE> header_buf_{};
    size_t header_pos_ = 0;
    u8 method_ = 0;
    u32 unpacked_size_ = 0;
    u32 packed_size_ = 0;
    u16 unpacked_crc_ = 0;
    u16 packed_crc_ = 0;

    // Bit buffer - Method 1 uses 32-bit LSB-first, Method 2 uses 8-bit MSB-first
    u32 bit_buffer_ = 0;
    unsigned bits_left_ = 0;

    // For method 1: track byte position for literal access
    const byte* m1_data_ = nullptr;
    size_t m1_byte_pos_ = 0;
    size_t m1_data_size_ = 0;

    // History buffer for back-references
    std::vector<u8> window_;
    size_t window_pos_ = 0;
    size_t total_output_ = 0;

    // Huffman tables for method 1
    std::array<huf_entry, 16> raw_table_{};
    std::array<huf_entry, 16> len_table_{};
    std::array<huf_entry, 16> pos_table_{};

    // Current table being built
    std::array<huf_entry, 16>* current_table_ = nullptr;
    unsigned table_count_ = 0;
    unsigned table_index_ = 0;

    // Decoding state
    u32 subchunk_count_ = 0;
    u32 raw_length_ = 0;
    u32 match_offset_ = 0;
    u32 match_count_ = 0;
    u32 literal_count_ = 0;

    // CRC calculation
    u16 computed_crc_ = 0;

    // For tracking packed data position for CRC verification
    size_t packed_data_start_ = 0;
    size_t packed_bytes_read_ = 0;

    // Encryption key (not fully supported, just parsed)
    bool locked_ = false;
    bool key_required_ = false;

    impl() : window_(WINDOW_SIZE, 0) {}

    void reset() {
        state_ = state::READ_HEADER;
        header_pos_ = 0;
        method_ = 0;
        unpacked_size_ = 0;
        packed_size_ = 0;
        unpacked_crc_ = 0;
        packed_crc_ = 0;
        bit_buffer_ = 0;
        bits_left_ = 0;
        m1_data_ = nullptr;
        m1_byte_pos_ = 0;
        m1_data_size_ = 0;
        std::fill(window_.begin(), window_.end(), 0);
        window_pos_ = 0;
        total_output_ = 0;
        subchunk_count_ = 0;
        raw_length_ = 0;
        match_offset_ = 0;
        match_count_ = 0;
        literal_count_ = 0;
        computed_crc_ = 0;
        packed_data_start_ = 0;
        packed_bytes_read_ = 0;
        locked_ = false;
        key_required_ = false;
    }

    // Update CRC for output bytes
    void update_output_crc(u8 b) {
        computed_crc_ = static_cast<u16>(
            (computed_crc_ >> 8) ^ rnc::crc_table[(computed_crc_ ^ b) & 0xFF]);
    }

    // Write byte to output and history
    void write_byte(u8 value, byte*& out_ptr, byte* out_end) {
        if (out_ptr < out_end) {
            *out_ptr++ = static_cast<byte>(value);
        }
        window_[window_pos_++ & (WINDOW_SIZE - 1)] = value;
        total_output_++;
        update_output_crc(value);
    }

    // Get byte from history
    u8 get_history(size_t distance) const {
        return window_[(window_pos_ - distance) & (WINDOW_SIZE - 1)];
    }

    // ========================= Method 1 bit operations (LSB-first) =========================
    // Method 1 uses a 32-bit buffer matching the reference implementation.
    // bit_buffer_ holds 32 bits: low 16 bits are "consumed", high 16 bits are lookahead.
    // bits_left_ tracks how many of the consumed 16 bits remain valid (16 down to 0).
    // When bits_left_ hits 0, we reload.

    // Read a byte from the source at current position
    u8 m1_read_byte() {
        if (m1_byte_pos_ < m1_data_size_) {
            return static_cast<u8>(m1_data_[m1_byte_pos_++]);
        }
        return 0;
    }

    // Peek a byte at offset from current position (for lookahead)
    u8 m1_peek_byte(size_t offset) const {
        if (m1_byte_pos_ + offset < m1_data_size_) {
            return static_cast<u8>(m1_data_[m1_byte_pos_ + offset]);
        }
        return 0;
    }

    // Reload the bit buffer (mimics reference implementation's input_bits_m1 reload)
    void m1_reload_buffer() {
        u8 b1 = m1_read_byte();
        u8 b2 = m1_read_byte();
        // Load 2 bytes consumed + 2 bytes lookahead
        bit_buffer_ = static_cast<u32>(b1) |
                      (static_cast<u32>(b2) << 8) |
                      (static_cast<u32>(m1_peek_byte(0)) << 16) |
                      (static_cast<u32>(m1_peek_byte(1)) << 24);
        bits_left_ = 16;  // 16 valid bits (2 consumed bytes)
    }

    // Read n bits from buffer (LSB-first), exactly like reference input_bits_m1
    u32 m1_read_bits(unsigned n) {
        u32 result = 0;
        u32 mask = 1;
        for (unsigned i = 0; i < n; i++) {
            if (bits_left_ == 0) {
                m1_reload_buffer();
            }
            if (bit_buffer_ & 1) {
                result |= mask;
            }
            bit_buffer_ >>= 1;
            mask <<= 1;
            bits_left_--;
        }
        return result;
    }

    // Refill buffer after literal copy (syncs lookahead)
    // Mimics: bit_buffer = (lookahead << bit_count) | (bit_buffer & ((1 << bit_count) - 1))
    void m1_refill_after_literals() {
        u32 remaining = bit_buffer_ & ((1u << bits_left_) - 1);
        u32 lookahead = static_cast<u32>(m1_peek_byte(0)) |
                        (static_cast<u32>(m1_peek_byte(1)) << 8) |
                        (static_cast<u32>(m1_peek_byte(2)) << 16);
        bit_buffer_ = remaining | (lookahead << bits_left_);
    }

    // ========================= Method 2 bit operations (MSB-first 8-bit) =========================

    bool m2_ensure_bits(const byte*& ptr, const byte* end, unsigned n) {
        while (bits_left_ < n) {
            if (ptr >= end) return false;
            // MSB-first: new byte goes into low bits, existing bits shift up
            bit_buffer_ = (bit_buffer_ << 8) | static_cast<u8>(*ptr++);
            bits_left_ += 8;
        }
        return true;
    }

    u32 m2_read_bits(unsigned n) {
        bits_left_ -= n;
        u32 val = (bit_buffer_ >> bits_left_) & ((1u << n) - 1);
        return val;
    }

    // ========================= Huffman table operations =========================

    // Build Huffman table from bit depths (method 1)
    void build_huffman_table(std::array<huf_entry, 16>& table, unsigned count) {
        // Build codes using canonical Huffman algorithm
        u32 code = 0;
        u32 div = 0x80000000u;
        for (unsigned bits = 1; bits <= 16; bits++) {
            for (unsigned i = 0; i < count; i++) {
                if (table[i].bit_depth == bits) {
                    // Reverse bits for LSB-first matching
                    u32 reversed = 0;
                    u32 val = code / div;
                    for (unsigned b = 0; b < bits; b++) {
                        reversed = (reversed << 1) | (val & 1);
                        val >>= 1;
                    }
                    table[i].code = reversed;
                    code += div;
                }
            }
            div >>= 1;
        }
    }

    // Decode a symbol using a Huffman table (method 1)
    // Mimics the reference implementation's decode_table_data exactly
    int m1_decode_symbol(const std::array<huf_entry, 16>& table) {
        // Ensure we have bits in the buffer
        if (bits_left_ == 0) {
            m1_reload_buffer();
        }

        // Search for matching code by testing each entry
        // The reference iterates through all entries looking for a match
        for (unsigned i = 0; i < 16; i++) {
            if (table[i].bit_depth == 0) continue;

            unsigned depth = table[i].bit_depth;

            // Check if low bits match this code
            // The bit_buffer_ has bits 0-15 as current, bits 16-31 as lookahead
            // So we can check up to 16 bits directly
            if ((bit_buffer_ & ((1u << depth) - 1)) == table[i].code) {
                // Consume the matched bits using m1_read_bits
                // This handles reload if we cross the 16-bit boundary
                m1_read_bits(depth);

                // If index < 2, return directly
                if (i < 2) return static_cast<int>(i);

                // Otherwise, read additional bits to get the full value
                unsigned extra_bits = i - 1;
                return static_cast<int>((1u << extra_bits) | m1_read_bits(extra_bits));
            }
        }
        return -1; // Invalid code (no match found)
    }

    result_t<stream_result> decompress(
        byte_span input,
        mutable_byte_span output,
        bool input_finished
    ) {
        const byte* in_ptr = input.data();
        const byte* in_end = input.data() + input.size();
        byte* out_ptr = output.data();
        byte* out_end = output.data() + output.size();

        while (state_ != state::DONE) {
            switch (state_) {
            case state::READ_HEADER:
                while (header_pos_ < rnc::HEADER_SIZE) {
                    if (in_ptr >= in_end) goto need_input;
                    header_buf_[header_pos_++] = static_cast<u8>(*in_ptr++);
                }
                {
                    // Parse header
                    u32 sig = (static_cast<u32>(header_buf_[0]) << 24) |
                              (static_cast<u32>(header_buf_[1]) << 16) |
                              (static_cast<u32>(header_buf_[2]) << 8) |
                              header_buf_[3];

                    if ((sig >> 8) != rnc::SIGNATURE) {
                        return std::unexpected(error{
                            error_code::InvalidSignature,
                            "Not a valid RNC file"
                        });
                    }

                    method_ = static_cast<u8>(sig & 0xFF);
                    if (method_ > 2) {
                        return std::unexpected(error(
                            error_code::UnsupportedCompression,
                            "Unsupported RNC method"
                        ));
                    }

                    unpacked_size_ = (static_cast<u32>(header_buf_[4]) << 24) |
                                     (static_cast<u32>(header_buf_[5]) << 16) |
                                     (static_cast<u32>(header_buf_[6]) << 8) |
                                     header_buf_[7];

                    packed_size_ = (static_cast<u32>(header_buf_[8]) << 24) |
                                   (static_cast<u32>(header_buf_[9]) << 16) |
                                   (static_cast<u32>(header_buf_[10]) << 8) |
                                   header_buf_[11];

                    unpacked_crc_ = static_cast<u16>(
                        (static_cast<u16>(header_buf_[12]) << 8) | header_buf_[13]);
                    packed_crc_ = static_cast<u16>(
                        (static_cast<u16>(header_buf_[14]) << 8) | header_buf_[15]);

                    // Bytes 16-17 are leeway and chunks_count (not needed for decompression)

                    packed_data_start_ = static_cast<size_t>(in_ptr - input.data());

                    // Method 0 is stored (uncompressed)
                    if (method_ == 0) {
                        raw_length_ = unpacked_size_;
                        state_ = state::M1_COPY_LITERALS;  // Reuse literal copy
                        break;
                    }
                }
                state_ = state::READ_INIT_BITS;
                break;

            case state::READ_INIT_BITS:
                if (method_ == 1) {
                    // Method 1 requires all input to be available (complex bit buffer)
                    if (static_cast<size_t>(in_end - in_ptr) < packed_size_) {
                        goto need_input;
                    }
                    // Initialize method 1 byte tracking
                    m1_data_ = in_ptr;
                    m1_byte_pos_ = 0;
                    m1_data_size_ = packed_size_;

                    // Read initial bits (lock flag, key flag)
                    locked_ = m1_read_bits(1) != 0;
                    key_required_ = m1_read_bits(1) != 0;
                    state_ = state::M1_READ_RAW_TABLE;
                } else {
                    // Method 2
                    if (!m2_ensure_bits(in_ptr, in_end, 2)) goto need_input;
                    locked_ = m2_read_bits(1) != 0;
                    key_required_ = m2_read_bits(1) != 0;
                    state_ = state::M2_DECODE_SYMBOL;
                }

                if (key_required_) {
                    return std::unexpected(error(
                        error_code::PasswordRequired,
                        "RNC file is encrypted (key required)"
                    ));
                }
                break;

            // ========================= Method 1 states =========================

            case state::M1_READ_RAW_TABLE:
                current_table_ = &raw_table_;
                table_index_ = 0;
                table_count_ = m1_read_bits(5);
                if (table_count_ > 16) table_count_ = 16;
                for (auto& e : *current_table_) {
                    e.code = 0;
                    e.bit_depth = 0;
                }
                [[fallthrough]];

            case state::M1_READ_LEN_TABLE:
            case state::M1_READ_POS_TABLE:
                // Read bit depths for current table
                while (table_index_ < table_count_) {
                    (*current_table_)[table_index_].bit_depth = static_cast<u16>(m1_read_bits(4));
                    table_index_++;
                }
                build_huffman_table(*current_table_, table_count_);

                if (state_ == state::M1_READ_RAW_TABLE || current_table_ == &raw_table_) {
                    current_table_ = &len_table_;
                    table_index_ = 0;
                    table_count_ = m1_read_bits(5);
                    if (table_count_ > 16) table_count_ = 16;
                    for (auto& e : *current_table_) {
                        e.code = 0;
                        e.bit_depth = 0;
                    }
                    state_ = state::M1_READ_LEN_TABLE;
                    break;
                } else if (current_table_ == &len_table_) {
                    current_table_ = &pos_table_;
                    table_index_ = 0;
                    table_count_ = m1_read_bits(5);
                    if (table_count_ > 16) table_count_ = 16;
                    for (auto& e : *current_table_) {
                        e.code = 0;
                        e.bit_depth = 0;
                    }
                    state_ = state::M1_READ_POS_TABLE;
                    break;
                }
                state_ = state::M1_READ_SUBCHUNK_COUNT;
                break;

            case state::M1_READ_SUBCHUNK_COUNT:
                subchunk_count_ = m1_read_bits(16);
                state_ = state::M1_DECODE_RAW_LENGTH;
                break;

            case state::M1_DECODE_RAW_LENGTH:
                {
                    int val = m1_decode_symbol(raw_table_);
                    if (val < 0) {
                        return std::unexpected(error(
                            error_code::CorruptData,
                            "RNC: Invalid Huffman code in raw table"
                        ));
                    }
                    raw_length_ = static_cast<u32>(val);
                }
                state_ = state::M1_COPY_LITERALS;
                break;

            case state::M1_COPY_LITERALS:
                if (method_ == 0) {
                    // Method 0: copy directly from input stream (stored data)
                    while (raw_length_ > 0) {
                        if (out_ptr >= out_end) goto need_output;
                        if (in_ptr >= in_end) goto need_input;
                        u8 literal = static_cast<u8>(*in_ptr++);
                        write_byte(literal, out_ptr, out_end);
                        raw_length_--;
                    }
                    state_ = state::DONE;
                    break;
                }

                // Method 1: read literals from m1_byte_pos_
                while (raw_length_ > 0) {
                    if (out_ptr >= out_end) goto need_output;
                    u8 literal = m1_read_byte();
                    write_byte(literal, out_ptr, out_end);
                    raw_length_--;
                }

                // Refill bit buffer with lookahead after literal copy
                m1_refill_after_literals();
                state_ = state::M1_CHECK_SUBCHUNK;
                break;

            case state::M1_CHECK_SUBCHUNK:
                subchunk_count_--;
                if (subchunk_count_ == 0) {
                    // Check if we're done
                    if (total_output_ >= unpacked_size_) {
                        state_ = state::DONE;
                    } else {
                        // Read next chunk's tables
                        state_ = state::M1_READ_RAW_TABLE;
                    }
                } else {
                    state_ = state::M1_DECODE_OFFSET;
                }
                break;

            case state::M1_DECODE_OFFSET:
                {
                    int val = m1_decode_symbol(len_table_);
                    if (val < 0) {
                        return std::unexpected(error(
                            error_code::CorruptData,
                            "RNC: Invalid Huffman code in length table"
                        ));
                    }
                    match_offset_ = static_cast<u32>(val) + 1;
                }
                state_ = state::M1_DECODE_LENGTH;
                break;

            case state::M1_DECODE_LENGTH:
                {
                    int val = m1_decode_symbol(pos_table_);
                    if (val < 0) {
                        return std::unexpected(error(
                            error_code::CorruptData,
                            "RNC: Invalid Huffman code in position table"
                        ));
                    }
                    match_count_ = static_cast<u32>(val) + 2;
                }
                state_ = state::M1_COPY_MATCH;
                break;

            case state::M1_COPY_MATCH:
                while (match_count_ > 0) {
                    if (out_ptr >= out_end) goto need_output;
                    u8 value = get_history(match_offset_);
                    write_byte(value, out_ptr, out_end);
                    match_count_--;
                }
                state_ = state::M1_DECODE_RAW_LENGTH;
                break;

            // ========================= Method 2 states =========================

            case state::M2_DECODE_SYMBOL:
                if (!m2_ensure_bits(in_ptr, in_end, 1)) goto need_input;
                if (m2_read_bits(1) == 0) {
                    // Literal byte
                    state_ = state::M2_COPY_LITERAL;
                } else {
                    // Match or special
                    state_ = state::M2_DECODE_MATCH_COUNT_1;
                }
                break;

            case state::M2_COPY_LITERAL:
                if (out_ptr >= out_end) goto need_output;
                if (in_ptr >= in_end) goto need_input;
                {
                    u8 literal = static_cast<u8>(*in_ptr++);
                    write_byte(literal, out_ptr, out_end);
                }
                state_ = state::M2_DECODE_SYMBOL;
                break;

            case state::M2_DECODE_MATCH_COUNT_1:
                if (!m2_ensure_bits(in_ptr, in_end, 1)) goto need_input;
                if (m2_read_bits(1) == 0) {
                    // 10 prefix: match with count decoding
                    state_ = state::M2_DECODE_MATCH_COUNT_2;
                } else {
                    // 11 prefix
                    state_ = state::M2_DECODE_MATCH_COUNT_4;
                }
                break;

            case state::M2_DECODE_MATCH_COUNT_2:
                // Decode match count using variable-length prefix
                if (!m2_ensure_bits(in_ptr, in_end, 1)) goto need_input;
                match_count_ = m2_read_bits(1) + 4; // Base count 4 or 5
                state_ = state::M2_DECODE_MATCH_COUNT_3;
                break;

            case state::M2_DECODE_MATCH_COUNT_3:
                if (!m2_ensure_bits(in_ptr, in_end, 1)) goto need_input;
                if (m2_read_bits(1)) {
                    // Further extend
                    if (!m2_ensure_bits(in_ptr, in_end, 1)) goto need_input;
                    match_count_ = ((match_count_ - 1) << 1) + m2_read_bits(1);
                }

                if (match_count_ == 9) {
                    // Special case: long literal run
                    state_ = state::M2_DECODE_LONG_LITERAL_COUNT;
                } else {
                    state_ = state::M2_DECODE_MATCH_OFFSET_1;
                }
                break;

            case state::M2_DECODE_MATCH_COUNT_4:
                // 11 prefix: check next bit
                if (!m2_ensure_bits(in_ptr, in_end, 1)) goto need_input;
                if (m2_read_bits(1) == 0) {
                    // 110: match with count 2, short offset (just read low byte)
                    match_count_ = 2;
                    match_offset_ = 0;  // No high bits for this path
                    state_ = state::M2_DECODE_MATCH_OFFSET_5;
                } else {
                    // 111x
                    state_ = state::M2_DECODE_MATCH_COUNT_5;
                }
                break;

            case state::M2_DECODE_MATCH_COUNT_5:
                if (!m2_ensure_bits(in_ptr, in_end, 1)) goto need_input;
                if (m2_read_bits(1)) {
                    // 1111: extended count from byte
                    if (in_ptr >= in_end) goto need_input;
                    match_count_ = static_cast<u8>(*in_ptr++) + 8;
                    if (match_count_ == 8) {
                        // End marker - read 1 more bit then done with chunk
                        state_ = state::M2_CHECK_END;
                        break;
                    }
                } else {
                    // 1110: count 3
                    match_count_ = 3;
                }
                state_ = state::M2_DECODE_MATCH_OFFSET_1;
                break;

            case state::M2_DECODE_MATCH_OFFSET_1:
                // Decode variable-length match offset high bits
                match_offset_ = 0;
                if (!m2_ensure_bits(in_ptr, in_end, 1)) goto need_input;
                if (m2_read_bits(1)) {
                    state_ = state::M2_DECODE_MATCH_OFFSET_2;
                } else {
                    // Short offset (low byte only)
                    state_ = state::M2_DECODE_MATCH_OFFSET_5;
                }
                break;

            case state::M2_DECODE_MATCH_OFFSET_2:
                if (!m2_ensure_bits(in_ptr, in_end, 1)) goto need_input;
                match_offset_ = m2_read_bits(1);
                state_ = state::M2_DECODE_MATCH_OFFSET_3;
                break;

            case state::M2_DECODE_MATCH_OFFSET_3:
                if (!m2_ensure_bits(in_ptr, in_end, 1)) goto need_input;
                if (m2_read_bits(1)) {
                    // Extended offset
                    if (!m2_ensure_bits(in_ptr, in_end, 1)) goto need_input;
                    match_offset_ = ((match_offset_ << 1) | m2_read_bits(1)) | 4;
                    state_ = state::M2_DECODE_MATCH_OFFSET_4;
                } else if (match_offset_ == 0) {
                    if (!m2_ensure_bits(in_ptr, in_end, 1)) goto need_input;
                    match_offset_ = m2_read_bits(1) + 2;
                    state_ = state::M2_DECODE_MATCH_OFFSET_5;
                } else {
                    state_ = state::M2_DECODE_MATCH_OFFSET_5;
                }
                break;

            case state::M2_DECODE_MATCH_OFFSET_4:
                if (!m2_ensure_bits(in_ptr, in_end, 1)) goto need_input;
                if (m2_read_bits(1) == 0) {
                    if (!m2_ensure_bits(in_ptr, in_end, 1)) goto need_input;
                    match_offset_ = (match_offset_ << 1) | m2_read_bits(1);
                }
                state_ = state::M2_DECODE_MATCH_OFFSET_5;
                break;

            case state::M2_DECODE_MATCH_OFFSET_5:
                // Read low byte of offset
                if (in_ptr >= in_end) goto need_input;
                match_offset_ = (match_offset_ << 8) | static_cast<u8>(*in_ptr++);
                match_offset_++; // Offsets are 1-based
                state_ = state::M2_COPY_MATCH;
                break;

            case state::M2_DECODE_LONG_LITERAL_COUNT:
                if (!m2_ensure_bits(in_ptr, in_end, 4)) goto need_input;
                literal_count_ = (m2_read_bits(4) << 2) + 12;
                state_ = state::M2_COPY_LONG_LITERALS;
                break;

            case state::M2_COPY_LONG_LITERALS:
                while (literal_count_ > 0) {
                    if (out_ptr >= out_end) goto need_output;
                    if (in_ptr >= in_end) goto need_input;
                    u8 literal = static_cast<u8>(*in_ptr++);
                    write_byte(literal, out_ptr, out_end);
                    literal_count_--;
                }
                state_ = state::M2_DECODE_SYMBOL;
                break;

            case state::M2_COPY_MATCH:
                while (match_count_ > 0) {
                    if (out_ptr >= out_end) goto need_output;
                    u8 value = get_history(match_offset_);
                    write_byte(value, out_ptr, out_end);
                    match_count_--;
                }
                state_ = state::M2_DECODE_SYMBOL;
                break;

            case state::M2_CHECK_END:
                if (!m2_ensure_bits(in_ptr, in_end, 1)) goto need_input;
                if (m2_read_bits(1) == 0) {
                    // End of data
                    state_ = state::DONE;
                } else {
                    // Continue to next chunk
                    state_ = state::M2_DECODE_SYMBOL;
                }
                break;

            case state::DONE:
                break;
            }

            // Check if we've output enough
            if (total_output_ >= unpacked_size_ && state_ != state::DONE) {
                state_ = state::DONE;
            }
        }

        // Verify unpacked CRC
        if (computed_crc_ != unpacked_crc_) {
            return std::unexpected(error(
                error_code::InvalidChecksum,
                "RNC unpacked CRC mismatch"
            ));
        }

        // For method 1, update in_ptr to reflect bytes consumed
        if (method_ == 1) {
            in_ptr = m1_data_ + m1_byte_pos_;
        }

        return stream_result::done(
            static_cast<size_t>(in_ptr - input.data()),
            static_cast<size_t>(out_ptr - output.data())
        );

    need_output:
        return stream_result::need_output(
            static_cast<size_t>(in_ptr - input.data()),
            static_cast<size_t>(out_ptr - output.data())
        );

    need_input:
        if (input_finished) {
            return std::unexpected(error{
                error_code::TruncatedArchive,
                "Unexpected end of RNC data"
            });
        }
        return stream_result::need_input(
            static_cast<size_t>(in_ptr - input.data()),
            static_cast<size_t>(out_ptr - output.data())
        );
    }
};

rnc_decompressor::rnc_decompressor()
    : pimpl_(std::make_unique<impl>()) {
}

rnc_decompressor::~rnc_decompressor() = default;

result_t<stream_result> rnc_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    return pimpl_->decompress(input, output, input_finished);
}

void rnc_decompressor::reset() {
    pimpl_->reset();
}

}  // namespace crate
