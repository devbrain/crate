#pragma once

#include <crate/core/types.hh>
#include <array>
#include <cstring>

namespace crate {

// RAR compression constants from unrar sources
namespace rar {
    // Maximum quick decode bits
    constexpr unsigned MAX_QUICK_DECODE_BITS = 9;

    // RAR 5.x/7.x alphabet sizes
    constexpr unsigned NC = 306; // Literal/length alphabet
    constexpr unsigned DCB = 64; // Distance codes (base, up to 4 GB)
    constexpr unsigned DCX = 80; // Distance codes (extended, up to 1 TB)
    constexpr unsigned LDC = 16; // Low distance codes
    constexpr unsigned RC = 44; // Repeat codes
    constexpr unsigned BC = 20; // Bit length codes
    constexpr unsigned HUFF_TABLE_SIZEB = NC + DCB + RC + LDC; // 430
    constexpr unsigned HUFF_TABLE_SIZEX = NC + DCX + RC + LDC; // 446

    // RAR 3.x alphabet sizes
    constexpr unsigned NC30 = 299;
    constexpr unsigned DC30 = 60;
    constexpr unsigned LDC30 = 17;
    constexpr unsigned RC30 = 28;
    constexpr unsigned BC30 = 20;
    constexpr unsigned HUFF_TABLE_SIZE30 = NC30 + DC30 + RC30 + LDC30; // 404

    // RAR 2.x alphabet sizes
    constexpr unsigned NC20 = 298;
    constexpr unsigned DC20 = 48;
    constexpr unsigned RC20 = 28;
    constexpr unsigned BC20 = 19;
    constexpr unsigned MC20 = 257;

    // Largest table size
    constexpr unsigned LARGEST_TABLE_SIZE = 306;

    // Maximum LZ match length
    constexpr unsigned MAX_LZ_MATCH = 0x1001;
    constexpr unsigned MAX_INC_LZ_MATCH = MAX_LZ_MATCH + 3;
    constexpr unsigned MAX3_LZ_MATCH = 0x101;
    constexpr unsigned MAX3_INC_LZ_MATCH = MAX3_LZ_MATCH + 3;

    // Low distance repeat count
    constexpr unsigned LOW_DIST_REP_COUNT = 16;
}

// RAR bitstream input (MSB-first, big-endian style)
class CRATE_EXPORT rar_bit_input {
public:
    static constexpr size_t MAX_SIZE = 0x8000; // 32KB buffer

    rar_bit_input() = default;

    void init(byte_span data) {
        data_ = data;
        in_addr_ = 0;
        in_bit_ = 0;
    }

    void init_bit_input() {
        in_addr_ = 0;
        in_bit_ = 0;
    }

    // Advance by 'bits' bits
    void add_bits(unsigned bits) {
        bits += in_bit_;
        in_addr_ += bits >> 3;
        in_bit_ = bits & 7;
    }

    // Return 16 bits from current position (MSB-first)
    [[nodiscard]] unsigned get_bits() const {
        if (in_addr_ >= data_.size()) {
            return 0; // True EOF
        }
        // Build bit field from available bytes, padding with 0
        unsigned bit_field = static_cast<unsigned>(data_[in_addr_]) << 16;
        if (in_addr_ + 1 < data_.size()) {
            bit_field |= static_cast<unsigned>(data_[in_addr_ + 1]) << 8;
        }
        if (in_addr_ + 2 < data_.size()) {
            bit_field |= static_cast<unsigned>(data_[in_addr_ + 2]);
        }
        bit_field >>= (8 - in_bit_);
        return bit_field & 0xFFFF;
    }

    // Return 32 bits from current position
    [[nodiscard]] u32 get_bits32() const {
        if (in_addr_ >= data_.size()) {
            return 0; // True EOF
        }
        // Build bit field from available bytes, padding with 0
        u32 bit_field = static_cast<u32>(data_[in_addr_]) << 24;
        if (in_addr_ + 1 < data_.size()) {
            bit_field |= static_cast<u32>(data_[in_addr_ + 1]) << 16;
        }
        if (in_addr_ + 2 < data_.size()) {
            bit_field |= static_cast<u32>(data_[in_addr_ + 2]) << 8;
        }
        if (in_addr_ + 3 < data_.size()) {
            bit_field |= static_cast<u32>(data_[in_addr_ + 3]);
        }
        bit_field <<= in_bit_;
        if (in_addr_ + 4 < data_.size()) {
            bit_field |= static_cast<u32>(data_[in_addr_ + 4]) >> (8 - in_bit_);
        }
        return bit_field;
    }

    // Get single byte
    u8 get_byte() {
        if (in_addr_ >= data_.size()) {
            return 0;
        }
        return data_[in_addr_++];
    }

    [[nodiscard]] bool overflow(unsigned inc_ptr) const {
        return in_addr_ + inc_ptr >= data_.size();
    }

    [[nodiscard]] bool at_end() const {
        return in_addr_ >= data_.size();
    }

    [[nodiscard]] size_t position() const { return in_addr_; }
    [[nodiscard]] unsigned bit_position() const { return in_bit_; }

private:
    byte_span data_{};
    size_t in_addr_ = 0;
    unsigned in_bit_ = 0;
};

// Huffman decode table structure
struct CRATE_EXPORT rar_decode_table {
    unsigned max_num = 0; // Alphabet size
    std::array<unsigned, 16> decode_len{}; // Left-aligned code limits
    std::array<unsigned, 16> decode_pos{}; // Start positions per bit length
    unsigned quick_bits = 0;
    std::array<u8, 1 << rar::MAX_QUICK_DECODE_BITS> quick_len{};
    std::array<u16, 1 << rar::MAX_QUICK_DECODE_BITS> quick_num{};
    std::array<u16, rar::LARGEST_TABLE_SIZE> decode_num{};

    void clear() {
        max_num = 0;
        decode_len.fill(0);
        decode_pos.fill(0);
        quick_bits = 0;
        quick_len.fill(0);
        quick_num.fill(0);
        decode_num.fill(0);
    }
};

// Build Huffman decode tables from length table
inline void make_decode_tables(const u8* length_table, rar_decode_table& dec, unsigned size) {
    dec.max_num = size;

    // Count codes per bit length
    std::array<unsigned, 16> length_count{};
    for (unsigned i = 0; i < size; i++) {
        length_count[length_table[i] & 0xF]++;
    }
    length_count[0] = 0; // Don't count zero-length codes

    // Clear decode_num
    std::fill(dec.decode_num.begin(), dec.decode_num.begin() + size, u16(0));

    dec.decode_pos[0] = 0;
    dec.decode_len[0] = 0;

    unsigned upper_limit = 0;
    for (unsigned i = 1; i < 16; i++) {
        upper_limit += length_count[i];
        unsigned left_aligned = upper_limit << (16 - i);
        upper_limit *= 2;
        dec.decode_len[i] = left_aligned;
        dec.decode_pos[i] = dec.decode_pos[i - 1] + length_count[i - 1];
    }

    // Copy decode_pos for modification
    std::array<unsigned, 16> copy_decode_pos = dec.decode_pos;

    // Fill decode_num
    for (unsigned i = 0; i < size; i++) {
        u8 cur_bit_length = length_table[i] & 0xF;
        if (cur_bit_length != 0) {
            unsigned last_pos = copy_decode_pos[cur_bit_length];
            dec.decode_num[last_pos] = static_cast<u16>(i);
            copy_decode_pos[cur_bit_length]++;
        }
    }

    // Set quick decode bits based on alphabet size
    switch (size) {
        case rar::NC:
        case rar::NC20:
        case rar::NC30:
            dec.quick_bits = rar::MAX_QUICK_DECODE_BITS;
            break;
        default:
            dec.quick_bits = rar::MAX_QUICK_DECODE_BITS > 3 ? rar::MAX_QUICK_DECODE_BITS - 3 : 0;
            break;
    }

    unsigned quick_data_size = 1u << dec.quick_bits;
    unsigned cur_bit_length = 1;

    // Build quick decode tables
    for (unsigned code = 0; code < quick_data_size; code++) {
        unsigned bit_field = code << (16 - dec.quick_bits);

        // Find bit length for this code
        while (cur_bit_length < 16 && bit_field >= dec.decode_len[cur_bit_length]) {
            cur_bit_length++;
        }

        dec.quick_len[code] = static_cast<u8>(cur_bit_length);

        // Calculate position in alphabet
        unsigned dist = bit_field - dec.decode_len[cur_bit_length - 1];
        dist >>= (16 - cur_bit_length);

        unsigned pos;
        if (cur_bit_length < 16 && (pos = dec.decode_pos[cur_bit_length] + dist) < size) {
            dec.quick_num[code] = dec.decode_num[pos];
        } else {
            dec.quick_num[code] = 0;
        }
    }
}

// Decode a Huffman symbol
inline unsigned decode_number(rar_bit_input& inp, const rar_decode_table& dec) {
    unsigned bit_field = inp.get_bits() & 0xFFFE;

    // Quick decode path
    if (bit_field < dec.decode_len[dec.quick_bits]) {
        unsigned code = bit_field >> (16 - dec.quick_bits);
        inp.add_bits(dec.quick_len[code]);
        return dec.quick_num[code];
    }

    // Slow path: find actual bit length
    unsigned bits = 15;
    for (unsigned i = dec.quick_bits + 1; i < 15; i++) {
        if (bit_field < dec.decode_len[i]) {
            bits = i;
            break;
        }
    }

    inp.add_bits(bits);

    // Calculate distance from start code
    unsigned dist = bit_field - dec.decode_len[bits - 1];
    dist >>= (16 - bits);

    unsigned pos = dec.decode_pos[bits] + dist;
    if (pos >= dec.max_num) {
        pos = 0; // Safety for corrupt data
    }

    return dec.decode_num[pos];
}

// Convert slot to length (RAR 5.x)
inline unsigned slot_to_length(rar_bit_input& inp, unsigned slot) {
    unsigned lbits, length = 2;
    if (slot < 8) {
        lbits = 0;
        length += slot;
    } else {
        lbits = slot / 4 - 1;
        length += (4 | (slot & 3)) << lbits;
    }

    if (lbits > 0) {
        length += inp.get_bits() >> (16 - lbits);
        inp.add_bits(lbits);
    }
    return length;
}

// RAR 2.9/3.x block tables
struct CRATE_EXPORT rar_block_tables_30 {
    rar_decode_table ld; // Literals/lengths
    rar_decode_table dd; // Distances
    rar_decode_table ldd; // Low distance bits
    rar_decode_table rd; // Repeat distances
    rar_decode_table bd; // Bit lengths
};

// RAR 5.x block tables
struct CRATE_EXPORT rar_block_tables {
    rar_decode_table ld; // Literals
    rar_decode_table dd; // Distances
    rar_decode_table ldd; // Low distance
    rar_decode_table rd; // Repeat
    rar_decode_table bd; // Bit lengths
};

}  // namespace crate
