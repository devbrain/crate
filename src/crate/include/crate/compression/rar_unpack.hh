#pragma once

#include <crate/core/decompressor.hh>
#include <crate/compression/rar_ppm.hh>
#include <crate/compression/rar_filters.hh>
#include <crate/core/types.hh>
#include <array>
#include <memory>
#include <vector>
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
                unsigned bit_field = static_cast <unsigned>(data_[in_addr_]) << 16;
                if (in_addr_ + 1 < data_.size()) {
                    bit_field |= static_cast <unsigned>(data_[in_addr_ + 1]) << 8;
                }
                if (in_addr_ + 2 < data_.size()) {
                    bit_field |= static_cast <unsigned>(data_[in_addr_ + 2]);
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
                u32 bit_field = static_cast <u32>(data_[in_addr_]) << 24;
                if (in_addr_ + 1 < data_.size()) {
                    bit_field |= static_cast <u32>(data_[in_addr_ + 1]) << 16;
                }
                if (in_addr_ + 2 < data_.size()) {
                    bit_field |= static_cast <u32>(data_[in_addr_ + 2]) << 8;
                }
                if (in_addr_ + 3 < data_.size()) {
                    bit_field |= static_cast <u32>(data_[in_addr_ + 3]);
                }
                bit_field <<= in_bit_;
                if (in_addr_ + 4 < data_.size()) {
                    bit_field |= static_cast <u32>(data_[in_addr_ + 4]) >> (8 - in_bit_);
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
        std::array <unsigned, 16> decode_len{}; // Left-aligned code limits
        std::array <unsigned, 16> decode_pos{}; // Start positions per bit length
        unsigned quick_bits = 0;
        std::array <u8, 1 << rar::MAX_QUICK_DECODE_BITS> quick_len{};
        std::array <u16, 1 << rar::MAX_QUICK_DECODE_BITS> quick_num{};
        std::array <u16, rar::LARGEST_TABLE_SIZE> decode_num{};

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
        std::array <unsigned, 16> length_count{};
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
        std::array <unsigned, 16> copy_decode_pos = dec.decode_pos;

        // Fill decode_num
        for (unsigned i = 0; i < size; i++) {
            u8 cur_bit_length = length_table[i] & 0xF;
            if (cur_bit_length != 0) {
                unsigned last_pos = copy_decode_pos[cur_bit_length];
                dec.decode_num[last_pos] = static_cast <u16>(i);
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

            dec.quick_len[code] = static_cast <u8>(cur_bit_length);

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

    // RAR 1.5 Decompressor (Old RAR format, < v1.50)
    // Uses adaptive Huffman coding with 64KB sliding window
    // Based on unrar unpack15.cc algorithm
    class CRATE_EXPORT rar_15_decompressor : public decompressor {
        public:
            static constexpr size_t WINDOW_SIZE = 0x10000; // 64KB window
            static constexpr size_t WINDOW_MASK = WINDOW_SIZE - 1;

            rar_15_decompressor() {
                init_huff();
            }

            result_t <size_t> decompress(byte_span input, mutable_byte_span output) override {
                inp_.init(input);

                // Initialize window
                if (window_.size() < WINDOW_SIZE) {
                    window_.resize(WINDOW_SIZE);
                }
                std::fill(window_.begin(), window_.end(), u8(0));

                unp_ptr_ = 0;
                for (auto& d : old_dist_) d = 0;
                old_dist_ptr_ = 0;
                last_dist_ = 0;
                last_length_ = 0;

                // Reset state
                init_huff();
                stmode_ = false;
                lcount_ = 0;
                flags_cnt_ = 0;
                flag_buf_ = 0;
                avr_ln1_ = 0;
                avr_ln2_ = 0;
                avr_ln3_ = 0;
                avr_plc_ = 0;
                avr_plc_b_ = 0;
                num_huf_ = 0;
                nhfb_ = 0;
                max_dist3_ = 0x2001;

                size_t out_pos = 0;
                bool finished = false;

                while (!inp_.at_end() && out_pos < output.size() && !finished) {
                    // Get next flag bit
                    if (flags_cnt_ == 0) {
                        flag_buf_ = inp_.get_bits() >> 8;
                        inp_.add_bits(8);
                        flags_cnt_ = 8;
                    }
                    flags_cnt_--;

                    if ((flag_buf_ & 0x80) != 0) {
                        flag_buf_ <<= 1;

                        // Get another flag bit
                        if (flags_cnt_ == 0) {
                            flag_buf_ = inp_.get_bits() >> 8;
                            inp_.add_bits(8);
                            flags_cnt_ = 8;
                        }
                        flags_cnt_--;

                        if ((flag_buf_ & 0x80) != 0) {
                            flag_buf_ <<= 1;
                            // LongLZ
                            if (!long_lz(out_pos, output.size())) {
                                finished = true;
                            }
                        } else {
                            flag_buf_ <<= 1;
                            // ShortLZ
                            if (!short_lz(out_pos, output.size())) {
                                finished = true;
                            }
                        }
                    } else {
                        flag_buf_ <<= 1;
                        // HuffDecode - decode literal
                        if (!huff_decode(out_pos, output.size())) {
                            finished = true;
                        }
                    }
                }

                // Copy to output
                size_t copy_size = std::min(out_pos, output.size());
                for (size_t i = 0; i < copy_size; i++) {
                    output[i] = window_[i & WINDOW_MASK];
                }

                return copy_size;
            }

            void reset() override {
                window_.clear();
                unp_ptr_ = 0;
                for (auto& d : old_dist_) d = 0;
                old_dist_ptr_ = 0;
                last_dist_ = 0;
                last_length_ = 0;
                init_huff();
            }

        private:
            void init_huff() {
                // Initialize character sets with initial priority values
                for (unsigned i = 0; i < 256; i++) {
                    ch_set_[i] = static_cast <u16>(i << 8);
                    ch_set_b_[i] = static_cast <u16>(i << 8);
                }

                // Initialize place tables
                for (unsigned i = 0; i < 256; i++) {
                    place_[i] = place_b_[i] = place_c_[i] = 0;
                }

                // Initialize position counters
                for (unsigned i = 0; i < 16; i++) {
                    ntopl_[i] = 0;
                    ntopl_b_[i] = 0;
                    ntopl_c_[i] = 0;
                }

                // Set up initial decode tables
                corr_huff(ch_set_, ntopl_);
                corr_huff(ch_set_b_, ntopl_b_);
            }

            static void corr_huff(u16* ch_set, unsigned* ntopl) {
                // Reinitialize character set with priority distribution
                for (unsigned i = 7; i > 0; i--) {
                    for (unsigned j = 0; j < 32; j++) {
                        unsigned pos = (7 - i) * 32 + j;
                        if (pos < 256) {
                            ch_set[pos] = static_cast <u16>((ch_set[pos] & 0xFF00) |
                                                           static_cast <u16>(i));
                        }
                    }
                }
                // Clear position counters
                for (unsigned i = 0; i < 16; i++) {
                    ntopl[i] = 0;
                }
            }

            unsigned decode_num(const unsigned* dec_tab, const unsigned* pos_tab) {
                // Decode a variable-length number using provided tables
                unsigned bits = inp_.get_bits() & 0xFFF0;

                // Find bit length
                unsigned i = 0;
                while (i < 8 && bits >= dec_tab[i]) {
                    i++;
                }

                unsigned start_pos = i < 2 ? 1 : ((i < 4) ? 2 : ((i < 6) ? 3 : 4));

                inp_.add_bits(start_pos);

                unsigned num = i > 0 ? dec_tab[i - 1] : 0;
                return ((bits - num) >> (16 - start_pos)) + pos_tab[start_pos];
            }

            bool short_lz(size_t& out_pos, size_t max_out) {
                // Static decode tables for short distances
                static const unsigned short_dec[] = {
                    0x0000, 0x4000, 0x8000, 0xa000, 0xc000, 0xd000, 0xe000, 0xf000
                };
                static const unsigned short_pos[] = {0, 0, 0, 1, 2, 3, 4, 5, 6};

                if (lcount_ == 2) {
                    // Repeat last match
                    lcount_ = 0;
                    copy_string(last_length_, last_dist_, out_pos, max_out);
                    return true;
                }

                unsigned bits = inp_.get_bits();

                // Decode length using short table
                unsigned length;
                if (bits < 0x8000) {
                    lcount_ = 0;
                    if (avr_ln1_ < 37) {
                        length = (bits >> 12) + 2;
                        inp_.add_bits(4);
                    } else {
                        length = (bits >> 11) + 2;
                        inp_.add_bits(5);
                    }
                } else {
                    inp_.add_bits(1);
                    bits = inp_.get_bits();

                    if (bits < 0x4000) {
                        length = 2;
                        inp_.add_bits(2);
                    } else if (bits < 0x8000) {
                        length = 3;
                        inp_.add_bits(2);
                    } else if (bits < 0xC000) {
                        length = 4;
                        inp_.add_bits(2);
                    } else {
                        length = 5;
                        inp_.add_bits(2);
                    }
                    lcount_ = length == 5 ? 2 : 0;
                }

                // Update averages
                avr_ln1_ = (avr_ln1_ * 3 + length) / 4;

                // Decode distance
                bits = inp_.get_bits();
                unsigned dist_slot = 0;
                for (unsigned i = 0; i < 8; i++) {
                    if (bits < short_dec[i]) {
                        break;
                    }
                    dist_slot = i + 1;
                }

                unsigned dist_bits = dist_slot < 2 ? 1 : (dist_slot < 4 ? 2 : (dist_slot < 6 ? 3 : 4));
                inp_.add_bits(dist_bits);

                unsigned distance;
                if (dist_slot < 2) {
                    distance = dist_slot + 1;
                } else {
                    unsigned extra = inp_.get_bits() >> (16 - (dist_slot / 2));
                    inp_.add_bits(dist_slot / 2);
                    distance = short_pos[dist_slot + 1] + extra + 1;
                }

                // Store distance and length
                insert_old_dist(distance);
                last_dist_ = distance;
                last_length_ = length;

                copy_string(length, distance, out_pos, max_out);
                return true;
            }

            bool long_lz(size_t& out_pos, size_t max_out) {
                // Decode length
                unsigned length;
                unsigned bits = inp_.get_bits();

                if (avr_ln2_ < 122) {
                    if (bits < 0x8000) {
                        length = (bits >> 12) + 3;
                        inp_.add_bits(4);
                    } else {
                        length = 3;
                        inp_.add_bits(1);
                    }
                } else if (avr_ln2_ < 64) {
                    if (bits < 0x4000) {
                        length = (bits >> 10) + 3;
                        inp_.add_bits(6);
                    } else {
                        length = 3;
                        inp_.add_bits(2);
                    }
                } else {
                    if (bits < 0x1000) {
                        length = (bits >> 8) + 3;
                        inp_.add_bits(8);
                    } else {
                        length = (bits >> 12) + 3;
                        inp_.add_bits(4);
                    }
                }

                // Decode distance place
                static const unsigned long_dec[] = {
                    0x0000, 0x4000, 0x6000, 0x8000, 0xa000, 0xb000, 0xc000, 0xd000
                };
                static const unsigned long_pos[] = {0, 0, 1, 2, 3, 4, 5, 6, 7, 8};

                bits = inp_.get_bits();
                unsigned place = 0;
                for (unsigned i = 0; i < 8; i++) {
                    if (bits < long_dec[i]) {
                        break;
                    }
                    place = i + 1;
                }

                unsigned place_bits = place < 2 ? 1 : (place < 4 ? 2 : (place < 6 ? 3 : 4));
                inp_.add_bits(place_bits);

                // Get distance from place
                unsigned dist_idx = long_pos[place + 1];
                if (place >= 2) {
                    unsigned extra = inp_.get_bits() >> (16 - (place / 2));
                    inp_.add_bits(place / 2);
                    dist_idx += extra;
                }

                unsigned distance;
                if (dist_idx < 4) {
                    distance = old_dist_[dist_idx];
                    // Reorder old distances
                    for (unsigned i = dist_idx; i > 0; i--) {
                        old_dist_[i] = old_dist_[i - 1];
                    }
                    old_dist_[0] = distance;
                } else {
                    // Read new distance (high bits from ChSetB)
                    unsigned high = (ch_set_b_[dist_idx - 4] >> 8) & 0xFF;

                    // Read low 8 bits
                    unsigned low = inp_.get_bits() >> 8;
                    inp_.add_bits(8);

                    distance = (high << 8) + low + 1;
                    insert_old_dist(distance);
                }

                // Adjust length based on distance
                if (distance >= max_dist3_) {
                    length++;
                }
                if (distance <= 256) {
                    length += 8;
                }

                // Update averages
                avr_ln2_ = (avr_ln2_ * 3 + length) / 4;

                last_dist_ = distance;
                last_length_ = length;

                copy_string(length, distance, out_pos, max_out);
                return true;
            }

            bool huff_decode(size_t& out_pos, size_t max_out) {
                // Static decode tables for Huffman
                static const unsigned hf_dec[] = {
                    0x0000, 0x8000, 0xc000, 0xe000, 0xf000, 0xf800, 0xfc00, 0xfe00
                };
                static const unsigned hf_pos[] = {0, 0, 1, 2, 3, 4, 5, 6, 7, 8};

                unsigned bits = inp_.get_bits();

                // Select Huffman table based on averages
                unsigned byte_val;
                unsigned hf_idx = 0;

                // Find matching Huffman entry
                for (unsigned i = 0; i < 8; i++) {
                    if (bits < hf_dec[i]) {
                        break;
                    }
                    hf_idx = i + 1;
                }

                unsigned hf_bits = hf_idx < 2 ? 1 : (hf_idx < 4 ? 2 : (hf_idx < 6 ? 3 : 4));
                inp_.add_bits(hf_bits);

                unsigned pos = hf_pos[hf_idx + 1];
                if (hf_idx >= 2) {
                    unsigned extra = inp_.get_bits() >> (16 - (hf_idx / 2));
                    inp_.add_bits(hf_idx / 2);
                    pos += extra;
                }

                // Get byte from character set
                byte_val = (ch_set_[pos] >> 8) & 0xFF;

                // Update character set frequency
                if (++ntopl_[place_[byte_val]] >= 0x80) {
                    corr_huff(ch_set_, ntopl_);
                } else {
                    // Move more frequent items up
                    if (place_[byte_val] < 255) {
                        place_[byte_val]++;
                    }
                }

                // Check for stream mode termination
                if (stmode_) {
                    if (byte_val == 0 && num_huf_++ >= 16) {
                        bits = inp_.get_bits();
                        if (bits > 0xFFF0) {
                            return false; // End of stream
                        }
                    }
                }

                // Output byte
                if (out_pos < max_out) {
                    window_[unp_ptr_++ & WINDOW_MASK] = static_cast <u8>(byte_val);
                    out_pos++;
                }

                return true;
            }

            void insert_old_dist(unsigned distance) {
                for (int i = 3; i > 0; i--) {
                    old_dist_[i] = old_dist_[i - 1];
                }
                old_dist_[0] = distance;
            }

            void copy_string(unsigned length, unsigned distance, size_t& out_pos, size_t max_out) {
                size_t src_ptr = unp_ptr_ - distance;
                while (length-- > 0 && out_pos < max_out) {
                    u8 value = window_[src_ptr++ & WINDOW_MASK];
                    window_[unp_ptr_++ & WINDOW_MASK] = value;
                    out_pos++;
                }
            }

            rar_bit_input inp_;
            std::vector <u8> window_;
            size_t unp_ptr_ = 0;

            unsigned old_dist_[4] = {0};
            unsigned old_dist_ptr_ = 0;
            unsigned last_dist_ = 0;
            unsigned last_length_ = 0;

            // Adaptive Huffman tables
            u16 ch_set_[256]; // Main character set
            u16 ch_set_b_[256]; // Distance place set

            // Place/frequency tracking
            u8 place_[256] = {0};
            u8 place_b_[256] = {0};
            u8 place_c_[256] = {0};
            unsigned ntopl_[16] = {0};
            unsigned ntopl_b_[16] = {0};
            unsigned ntopl_c_[16] = {0};

            // State variables
            bool stmode_ = false;
            unsigned lcount_ = 0;
            unsigned flags_cnt_ = 0;
            unsigned flag_buf_ = 0;
            unsigned avr_ln1_ = 0;
            unsigned avr_ln2_ = 0;
            unsigned avr_ln3_ = 0;
            unsigned avr_plc_ = 0;
            unsigned avr_plc_b_ = 0;
            unsigned num_huf_ = 0;
            unsigned nhfb_ = 0;
            unsigned max_dist3_ = 0x2001;
    };

    // RAR 5.x block tables
    struct CRATE_EXPORT rar_block_tables {
        rar_decode_table ld; // Literals
        rar_decode_table dd; // Distances
        rar_decode_table ldd; // Low distance
        rar_decode_table rd; // Repeat
        rar_decode_table bd; // Bit lengths
    };

    // RAR 2.9/3.x Decompressor (LZ77 + PPMd support)
    class CRATE_EXPORT rar_29_decompressor : public decompressor {
        public:
            rar_29_decompressor() {
                init_tables();
            }

            // Enable/disable solid mode (preserves window between files)
            void set_solid_mode(bool solid) { solid_mode_ = solid; }
            bool is_solid_mode() const { return solid_mode_; }

            result_t <size_t> decompress(byte_span input, mutable_byte_span output) override {
                inp_.init(input);
                input_span_ = input; // Store for later PPM use

                // Initialize window
                size_t min_size = std::max(output.size(), size_t(0x400000)); // Min 4MB window
                if (window_.size() < min_size) {
                    window_.resize(min_size);
                }

                // In solid mode, don't reset window contents and positions
                if (!solid_mode_) {
                    std::fill(window_.begin(), window_.end(), u8(0));
                    unp_ptr_ = 0;
                    wr_ptr_ = 0;
                    first_win_done_ = false;
                    tables_read_ = false;
                    ppm_mode_ = false;
                    ppm_esc_char_ = 2;
                    old_dist_.fill(size_t(-1));
                    old_dist_ptr_ = 0;
                    last_length_ = 0;
                    low_dist_rep_count_ = 0;
                    prev_low_dist_ = 0;
                }

                // Record start position for solid mode - this file's data starts here
                size_t solid_start = unp_ptr_;

                // Read tables (or init PPM) - always needed for each file
                if (!read_tables()) {
                    return std::unexpected(error{
                        error_code::InvalidHuffmanTable,
                        "Failed to read RAR3 Huffman tables"
                    });
                }

                // Static tables for short distance codes (263-271)
                static const u8 sd_decode[] = {0, 4, 8, 16, 32, 64, 128, 192};
                static const u8 sd_bits[] = {2, 2, 3, 4, 5, 6, 6, 6};

                // Decompress
                size_t out_pos = 0;
                while (out_pos < output.size()) {
                    // PPM mode decoding
                    if (ppm_mode_) {
                        int ch = ppm_model_.decode_char();
                        if (ch < 0) {
                            // PPM error - try to recover or end
                            ppm_model_.cleanup();
                            break;
                        }

                        if (ch == ppm_esc_char_) {
                            // Escape sequence - decode command
                            ch = ppm_model_.decode_char();
                            if (ch < 0) break;

                            if (ch == 0) {
                                // Switch to LZ mode
                                if (!read_tables()) break;
                                continue;
                            }

                            if (ch == 2) {
                                // End of PPM data
                                break;
                            }

                            if (ch == 3) {
                                // VM filter - not supported, skip
                                continue;
                            }

                            if (ch == 4) {
                                // Read new distance
                                unsigned dist = 0;
                                for (int i = 0; i < 4; i++) {
                                    ch = ppm_model_.decode_char();
                                    if (ch < 0) break;
                                    dist |= static_cast <unsigned>(ch) << (i * 8);
                                }
                                if (ch < 0) break;
                                insert_old_dist(dist + 1);
                                continue;
                            }

                            if (ch == 5) {
                                // One-byte distance
                                ch = ppm_model_.decode_char();
                                if (ch < 0) break;
                                insert_old_dist(static_cast <unsigned>(ch) + 1);
                                continue;
                            }

                            if (ch >= 6) {
                                // Length + distance slot
                                unsigned length = static_cast <unsigned>(ch) - 6 + 2;
                                if (length >= 3) {
                                    // Get extra length byte
                                    ch = ppm_model_.decode_char();
                                    if (ch < 0) break;
                                    length += static_cast <unsigned>(ch) << 2;
                                }
                                // Use old_dist_[0] for distance
                                if (old_dist_[0] != size_t(-1)) {
                                    out_pos += copy_string(length, old_dist_[0], output.size() - out_pos);
                                    last_length_ = length;
                                }
                                continue;
                            }
                        } else {
                            // Literal character
                            window_[unp_ptr_++] = static_cast <u8>(ch);
                            out_pos++;
                        }
                        continue;
                    }

                    // LZ77 mode decoding
                    if (inp_.at_end()) break;

                    unsigned number = decode_number(inp_, tables_.ld);

                    if (number < 256) {
                        // Literal byte
                        window_[unp_ptr_++] = static_cast <u8>(number);
                        out_pos++;
                    } else if (number >= 271) {
                        // Match (number >= 271)
                        unsigned length = ldecode_[number - 271] + 3;
                        unsigned bits = lbits_[number - 271];
                        if (bits > 0) {
                            length += inp_.get_bits() >> (16 - bits);
                            inp_.add_bits(bits);
                        }

                        unsigned dist_number = decode_number(inp_, tables_.dd);
                        unsigned distance = static_cast <unsigned>(ddecode_[dist_number] + 1);
                        bits = dbits_[dist_number];
                        if (bits > 0) {
                            if (dist_number > 9) {
                                if (bits > 4) {
                                    distance += ((inp_.get_bits() >> (20 - bits)) << 4);
                                    inp_.add_bits(bits - 4);
                                }
                                if (low_dist_rep_count_ > 0) {
                                    low_dist_rep_count_--;
                                    distance += prev_low_dist_;
                                } else {
                                    unsigned low_dist = decode_number(inp_, tables_.ldd);
                                    if (low_dist == 16) {
                                        low_dist_rep_count_ = rar::LOW_DIST_REP_COUNT - 1;
                                        distance += prev_low_dist_;
                                    } else {
                                        distance += low_dist;
                                        prev_low_dist_ = low_dist;
                                    }
                                }
                            } else {
                                distance += inp_.get_bits() >> (16 - bits);
                                inp_.add_bits(bits);
                            }
                        }

                        // Length adjustment for long distances
                        if (distance >= 0x2000) {
                            length++;
                            if (distance >= 0x40000) {
                                length++;
                            }
                        }

                        insert_old_dist(distance);
                        last_length_ = length;
                        out_pos += copy_string(length, distance, output.size() - out_pos);
                    } else if (number == 256) {
                        // End of block
                        if (!read_end_of_block()) {
                            break;
                        }
                    } else if (number == 257) {
                        // VM code / filter - skip for now
                        // In a full implementation, we'd read and process filter data
                        continue;
                    } else if (number == 258) {
                        // Repeat with last length and old_dist[0]
                        if (last_length_ != 0 && old_dist_[0] != size_t(-1)) {
                            out_pos += copy_string(last_length_, old_dist_[0], output.size() - out_pos);
                        }
                    } else if (number < 263) {
                        // Repeat with old_dist[number-259], length from RD table + 2
                        unsigned dist_num = number - 259;
                        if (dist_num >= old_dist_.size() || old_dist_[dist_num] == size_t(-1)) {
                            continue;
                        }

                        size_t distance = old_dist_[dist_num];

                        // Reorder old distances
                        for (unsigned i = dist_num; i > 0; i--) {
                            old_dist_[i] = old_dist_[i - 1];
                        }
                        old_dist_[0] = distance;

                        // Decode length from RD table
                        unsigned length_number = decode_number(inp_, tables_.rd);
                        unsigned length = ldecode_[length_number] + 2;
                        unsigned bits = lbits_[length_number];
                        if (bits > 0) {
                            length += inp_.get_bits() >> (16 - bits);
                            inp_.add_bits(bits);
                        }

                        last_length_ = length;
                        out_pos += copy_string(length, distance, output.size() - out_pos);
                    } else {
                        // Short distance match (263-271)
                        unsigned sd_num = number - 263;
                        unsigned distance = sd_decode[sd_num] + 1;
                        unsigned bits = sd_bits[sd_num];
                        if (bits > 0) {
                            distance += inp_.get_bits() >> (16 - bits);
                            inp_.add_bits(bits);
                        }

                        insert_old_dist(distance);
                        last_length_ = 2;
                        out_pos += copy_string(2, distance, output.size() - out_pos);
                    }
                }

                // Copy to output - in solid mode, copy from where this file started
                size_t copy_size = std::min(out_pos, output.size());
                size_t win_mask = window_.size() - 1; // Window is power of 2
                for (size_t i = 0; i < copy_size; i++) {
                    output[i] = window_[(solid_start + i) & win_mask];
                }
                return copy_size;
            }

            void reset() override {
                window_.clear();
                unp_ptr_ = 0;
                wr_ptr_ = 0;
                first_win_done_ = false;
                tables_read_ = false;
                old_dist_.fill(size_t(-1));
                old_dist_ptr_ = 0;
                last_length_ = 0;
                low_dist_rep_count_ = 0;
                prev_low_dist_ = 0;
                old_table_.fill(0);
                ppm_mode_ = false;
                ppm_esc_char_ = 2;
                solid_mode_ = false;
            }

        private:
            void init_tables() {
                // Initialize length decode tables
                ldecode_ = {
                    0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160,
                    192, 224
                };
                lbits_ = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5};

                // Initialize distance decode tables
                static const int dbit_length_counts[] = {4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 14, 0, 12};
                int dist = 0, bit_length = 0, slot = 0;
                const int max_slot = static_cast <int>(rar::DC30);
                for (int i = 0; i < 19 && slot < max_slot; i++, bit_length++) {
                    for (int j = 0; j < dbit_length_counts[i] && slot < max_slot;
                         j++, slot++, dist += (1 << bit_length)) {
                        size_t slot_index = static_cast <size_t>(slot);
                        ddecode_[slot_index] = dist;
                        dbits_[slot_index] = static_cast <u8>(bit_length);
                    }
                }
            }

            bool read_tables() {
                // Align to byte boundary
                inp_.add_bits((8 - inp_.bit_position()) & 7);

                // Check block type
                unsigned bit_field = inp_.get_bits();
                if (bit_field & 0x8000) {
                    // PPM block - initialize PPM decoder
                    inp_.add_bits(1);

                    // Create bit-stream input adapter at current bit position (NOT byte-aligned)
                    // PPM data starts immediately after the 1-bit marker
                    ppm_input_ = std::make_unique <rar::ppm::bit_stream_input>(
                        input_span_, inp_.position(), inp_.bit_position());

                    int esc_char = ppm_esc_char_;
                    if (!ppm_model_.decode_init(ppm_input_.get(), esc_char)) {
                        return false;
                    }
                    ppm_esc_char_ = esc_char;
                    ppm_mode_ = true;
                    return true;
                }

                // LZ mode
                ppm_mode_ = false;

                // Check if we should preserve old table
                if (!(bit_field & 0x4000)) {
                    old_table_.fill(0);
                }
                inp_.add_bits(2);

                // Read bit length table with special handling for 15
                std::array <u8, rar::BC30> bit_length{};
                for (unsigned i = 0; i < rar::BC30;) {
                    unsigned length = inp_.get_bits() >> 12;
                    inp_.add_bits(4);

                    if (length == 15) {
                        unsigned zero_count = inp_.get_bits() >> 12;
                        inp_.add_bits(4);
                        if (zero_count == 0) {
                            bit_length[i++] = 15;
                        } else {
                            zero_count += 2;
                            while (zero_count-- > 0 && i < rar::BC30) {
                                bit_length[i++] = 0;
                            }
                        }
                    } else {
                        bit_length[i++] = static_cast <u8>(length);
                    }
                }

                make_decode_tables(bit_length.data(), tables_.bd, rar::BC30);

                // Read main table (values are deltas from old table)
                std::array <u8, rar::HUFF_TABLE_SIZE30> table{};
                unsigned n = 0;
                while (n < rar::HUFF_TABLE_SIZE30) {
                    if (inp_.at_end()) break;

                    unsigned num = decode_number(inp_, tables_.bd);
                    if (num < 16) {
                        // Delta from old table
                        table[n] = static_cast <u8>((num + old_table_[n]) & 0xF);
                        n++;
                    } else if (num < 18) {
                        // 16: repeat prev 3-10 times (3 bits)
                        // 17: repeat prev 11-138 times (7 bits)
                        if (n == 0) return false;
                        unsigned count;
                        if (num == 16) {
                            count = (inp_.get_bits() >> 13) + 3;
                            inp_.add_bits(3);
                        } else {
                            // num == 17
                            count = (inp_.get_bits() >> 9) + 11;
                            inp_.add_bits(7);
                        }
                        while (count-- > 0 && n < rar::HUFF_TABLE_SIZE30) {
                            table[n] = table[n - 1];
                            n++;
                        }
                    } else {
                        // 18: zeros 3-10 times (3 bits)
                        // 19: zeros 11-138 times (7 bits)
                        unsigned count;
                        if (num == 18) {
                            count = (inp_.get_bits() >> 13) + 3;
                            inp_.add_bits(3);
                        } else {
                            // num == 19
                            count = (inp_.get_bits() >> 9) + 11;
                            inp_.add_bits(7);
                        }
                        while (count-- > 0 && n < rar::HUFF_TABLE_SIZE30) {
                            table[n++] = 0;
                        }
                    }
                }

                // Save for next block
                old_table_ = table;

                make_decode_tables(table.data(), tables_.ld, rar::NC30);
                make_decode_tables(table.data() + rar::NC30, tables_.dd, rar::DC30);
                make_decode_tables(table.data() + rar::NC30 + rar::DC30, tables_.ldd, rar::LDC30);
                make_decode_tables(table.data() + rar::NC30 + rar::DC30 + rar::LDC30, tables_.rd, rar::RC30);

                tables_read_ = true;
                return true;
            }

            bool read_end_of_block() {
                // End of block bit encoding:
                // "1"  - no new file, new table just here (continue)
                // "00" - new file, no new table (end)
                // "01" - new file, new table in next file (end)
                unsigned bit_field = inp_.get_bits();

                if ((bit_field & 0x8000) != 0) {
                    // Bit 1: new table here, continue with same file
                    inp_.add_bits(1);
                    return read_tables();
                }

                // Bits 00 or 01: new file (end of current file)
                // We don't support multi-file decompression, so just return false
                inp_.add_bits(2);
                return false;
            }

            void insert_old_dist(size_t distance) {
                old_dist_[3] = old_dist_[2];
                old_dist_[2] = old_dist_[1];
                old_dist_[1] = old_dist_[0];
                old_dist_[0] = distance;
            }

            unsigned copy_string(unsigned length, size_t distance, size_t max_out) {
                size_t src_ptr = unp_ptr_ - distance;
                if (distance > unp_ptr_) {
                    src_ptr += window_.size();
                }

                unsigned copied = 0;
                while (length-- > 0 && unp_ptr_ < window_.size() && copied < max_out) {
                    window_[unp_ptr_++] = window_[src_ptr++ % window_.size()];
                    copied++;
                }
                return copied;
            }

            rar_bit_input inp_;
            std::vector <u8> window_;
            size_t unp_ptr_ = 0;
            size_t wr_ptr_ = 0;
            bool first_win_done_ = false;
            bool tables_read_ = false;

            std::array <size_t, 4> old_dist_{};
            size_t old_dist_ptr_ = 0;
            unsigned last_length_ = 0;
            unsigned low_dist_rep_count_ = 0;
            unsigned prev_low_dist_ = 0;

            rar_block_tables_30 tables_;
            std::array <u8, rar::HUFF_TABLE_SIZE30> old_table_{}; // Previous Huffman table for delta encoding

            // Static decode tables
            std::array <u8, 28> ldecode_{};
            std::array <u8, 28> lbits_{};
            std::array <int, rar::DC30> ddecode_{};
            std::array <u8, rar::DC30> dbits_{};

            // PPM support
            bool ppm_mode_ = false;
            int ppm_esc_char_ = 2;
            rar::ppm::model_ppm ppm_model_;
            std::unique_ptr <rar::ppm::input_adapter> ppm_input_;
            byte_span input_span_; // Stored for creating PPM bit-stream adapters

            // Solid mode support
            bool solid_mode_ = false; // Preserve window between files (solid archive)
    };

    // RAR 5.x Decompressor
    class CRATE_EXPORT rar5_decompressor : public decompressor {
        public:
            rar5_decompressor(bool extra_dist = false)
                : extra_dist_(extra_dist) {
            }

            // Enable/disable solid mode (preserves window between files)
            void set_solid_mode(bool solid) { solid_mode_ = solid; }
            bool is_solid_mode() const { return solid_mode_; }

            result_t <size_t> decompress(byte_span input, mutable_byte_span output) override {
                inp_.init(input);

                // Initialize window
                size_t min_size = std::max(output.size() * 2, size_t(0x100000)); // Min 1MB
                if (window_.size() < min_size) {
                    window_.resize(min_size);
                }
                max_win_size_ = window_.size();
                max_win_mask_ = max_win_size_ - 1;

                // In solid mode, don't reset window contents and positions
                if (!solid_mode_) {
                    std::fill(window_.begin(), window_.end(), u8(0));
                    unp_ptr_ = 0;
                    wr_ptr_ = 0;
                    first_win_done_ = false;
                    old_dist_.fill(0);
                    last_length_ = 0;
                    tables_read_ = false;
                    filter_processor_.clear();
                    written_file_pos_ = 0;
                }

                // Record start position for solid mode - this file's data starts here
                size_t solid_start = unp_ptr_;
                size_t out_pos = 0;

                while (!inp_.at_end() && out_pos < output.size()) {
                    // Read block header
                    if (!read_block_header()) {
                        return std::unexpected(error{error_code::CorruptData, "Failed to read RAR5 block header"});
                    }

                    // Track block start BEFORE reading tables (tables are part of block content)
                    size_t block_start = inp_.position();
                    size_t block_bit_start = inp_.bit_position();

                    if (block_header_.table_present) {
                        if (!read_tables()) {
                            return std::unexpected(error{
                                error_code::InvalidHuffmanTable,
                                "Failed to read RAR5 Huffman tables"
                            });
                        }
                        tables_read_ = true;
                    }

                    if (!tables_read_) {
                        return std::unexpected(error{
                            error_code::InvalidHuffmanTable,
                            "No Huffman tables in RAR5 stream"
                        });
                    }

                    // Process block data
                    while (out_pos < output.size()) {
                        // Check if we've consumed the block
                        // Block content spans BlockSize byte positions with BlockBitSize bits in last byte
                        size_t bits_consumed = (inp_.position() - block_start) * 8 +
                                               inp_.bit_position() - block_bit_start;
                        size_t block_bits = static_cast <size_t>(block_header_.block_size - 1) * 8 +
                                            static_cast <size_t>(block_header_.block_bit_size);

                        if (bits_consumed >= block_bits) {
                            break;
                        }

                        unsigned number = decode_number(inp_, tables_.ld);

                        if (number < 256) {
                            // Literal
                            window_[unp_ptr_++ & max_win_mask_] = static_cast <u8>(number);
                            out_pos++;
                        } else if (number == 256) {
                            // Filter - read filter data and store for later application
                            auto read_vint = [this]() -> u64 {
                                unsigned byte_count = (inp_.get_bits() >> 14) + 1;
                                inp_.add_bits(2);
                                u64 data = 0;
                                for (unsigned i = 0; i < byte_count; i++) {
                                    data += static_cast <u64>(inp_.get_bits() >> 8) << (i * 8);
                                    inp_.add_bits(8);
                                }
                                return data;
                            };

                            u64 filter_block_start = read_vint();
                            u64 block_length = read_vint();

                            unsigned filter_type = inp_.get_bits() >> 13;
                            inp_.add_bits(3);

                            rar_filter filter;
                            filter.block_start = written_file_pos_ + filter_block_start;
                            filter.block_length = block_length;
                            filter.channels = 0;

                            switch (filter_type) {
                                case 0: // FILTER_DELTA
                                    filter.type = rar_filter_type::DELTA;
                                    filter.channels = static_cast <u8>((inp_.get_bits() >> 11) + 1);
                                    inp_.add_bits(5);
                                    break;
                                case 1: // FILTER_E8
                                    filter.type = rar_filter_type::E8;
                                    break;
                                case 2: // FILTER_E8E9
                                    filter.type = rar_filter_type::E8E9;
                                    break;
                                case 3: // FILTER_ARM
                                    filter.type = rar_filter_type::ARM;
                                    break;
                                default:
                                    // Unknown filter type - skip
                                    continue;
                            }

                            filter_processor_.add_filter(filter);
                            continue;
                        } else if (number == 257) {
                            // Repeat last length with old_dist[0]
                            if (last_length_ != 0) {
                                out_pos += copy_string(last_length_, old_dist_[0], output.size() - out_pos);
                            }
                        } else if (number < 262) {
                            // Short repeat (use previous distance 0-3)
                            unsigned dist_num = number - 258;
                            size_t distance = old_dist_[dist_num];
                            unsigned length = slot_to_length(inp_, decode_number(inp_, tables_.rd));

                            // Reorder distances
                            for (unsigned i = dist_num; i > 0; i--) {
                                old_dist_[i] = old_dist_[i - 1];
                            }
                            old_dist_[0] = distance;

                            out_pos += copy_string(length, distance, output.size() - out_pos);
                            last_length_ = length;
                        } else {
                            // Match with length slot
                            unsigned length_slot = number - 262;
                            unsigned length = slot_to_length(inp_, length_slot);

                            // Decode distance
                            unsigned dist_slot = decode_number(inp_, tables_.dd);
                            size_t distance = 1;
                            unsigned d_bits = 0;

                            if (dist_slot < 4) {
                                distance += dist_slot;
                            } else {
                                d_bits = dist_slot / 2 - 1;
                                distance += size_t(2 | (dist_slot & 1)) << d_bits;
                            }

                            // Read distance extra bits
                            if (d_bits > 0) {
                                if (d_bits >= 4) {
                                    if (d_bits > 4) {
                                        // Read upper bits (d_bits - 4), shifted left by 4
                                        unsigned extra = inp_.get_bits() >> (16 - (d_bits - 4));
                                        distance += extra << 4;
                                        inp_.add_bits(d_bits - 4);
                                    }
                                    // Read low distance bits (always for d_bits >= 4)
                                    unsigned low_dist = decode_number(inp_, tables_.ldd);
                                    distance += low_dist;
                                } else {
                                    // DBits < 4: just read d_bits bits
                                    unsigned extra = inp_.get_bits() >> (16 - d_bits);
                                    distance += extra;
                                    inp_.add_bits(d_bits);
                                }
                            }

                            // Add to length based on distance ranges
                            if (distance > 0x100) {
                                length++;
                                if (distance > 0x2000) {
                                    length++;
                                    if (distance > 0x40000) {
                                        length++;
                                    }
                                }
                            }

                            insert_old_dist(distance);
                            out_pos += copy_string(length, distance, output.size() - out_pos);
                            last_length_ = length;
                        }
                    }

                    if (block_header_.last_block) {
                        break;
                    }
                }

                // Copy to output - in solid mode, copy from where this file started
                size_t copy_size = std::min(out_pos, output.size());
                for (size_t i = 0; i < copy_size; i++) {
                    output[i] = window_[(solid_start + i) & max_win_mask_];
                }

                // Apply any pending filters to the output data
                filter_processor_.apply_filters(output.data(), copy_size, written_file_pos_);
                written_file_pos_ += copy_size;

                return copy_size;
            }

            void reset() override {
                window_.clear();
                unp_ptr_ = 0;
                wr_ptr_ = 0;
                first_win_done_ = false;
                old_dist_.fill(0);
                last_length_ = 0;
                tables_read_ = false;
                solid_mode_ = false;
                filter_processor_.clear();
                written_file_pos_ = 0;
            }

        private:
            struct CRATE_EXPORT BlockHeader {
                int block_size = 0;
                int block_bit_size = 0;
                bool last_block = false;
                bool table_present = false;
            };

            bool read_block_header() {
                if (inp_.at_end()) return false;

                // Align to byte boundary
                unsigned bit_offset = inp_.bit_position();
                if (bit_offset != 0) {
                    inp_.add_bits(8 - bit_offset);
                }

                // Read block flags (1 byte)
                u8 flags = static_cast <u8>(inp_.get_bits() >> 8);
                inp_.add_bits(8);

                // Parse flags:
                // bits 0-2: block bit size - 1
                // bits 3-4: byte count - 1 (for block size)
                // bit 6: last block in file
                // bit 7: table present
                block_header_.block_bit_size = (flags & 0x07) + 1;
                unsigned byte_count = ((flags >> 3) & 0x03) + 1;
                block_header_.last_block = (flags & 0x40) != 0;
                block_header_.table_present = (flags & 0x80) != 0;

                if (byte_count == 4) {
                    return false; // Invalid byte count
                }

                // Read checksum (1 byte)
                u8 saved_checksum = static_cast <u8>(inp_.get_bits() >> 8);
                inp_.add_bits(8);

                // Read block size (little-endian, byte_count bytes)
                int block_size = 0;
                for (unsigned i = 0; i < byte_count; i++) {
                    block_size += static_cast <int>(inp_.get_bits() >> 8) << (i * 8);
                    inp_.add_bits(8);
                }
                block_header_.block_size = block_size;

                // Verify checksum
                u8 checksum = 0x5A ^ flags ^ static_cast <u8>(block_size) ^
                              static_cast <u8>(block_size >> 8) ^ static_cast <u8>(block_size >> 16);
                if (checksum != saved_checksum) {
                    return false; // Checksum mismatch
                }

                return true;
            }

            bool read_tables() {
                // Read bit length codes with run-length encoding
                std::array <u8, rar::BC> bit_length{};
                for (unsigned i = 0; i < rar::BC;) {
                    unsigned len = inp_.get_bits() >> 12;
                    inp_.add_bits(4);

                    if (len == 15) {
                        unsigned zero_count = inp_.get_bits() >> 12;
                        inp_.add_bits(4);
                        if (zero_count == 0) {
                            bit_length[i++] = 15;
                        } else {
                            zero_count += 2;
                            while (zero_count-- > 0 && i < rar::BC) {
                                bit_length[i++] = 0;
                            }
                        }
                    } else {
                        bit_length[i++] = static_cast <u8>(len);
                    }
                }

                make_decode_tables(bit_length.data(), tables_.bd, rar::BC);

                // Read main table
                unsigned table_size = extra_dist_ ? rar::HUFF_TABLE_SIZEX : rar::HUFF_TABLE_SIZEB;
                std::vector <u8> table(table_size, 0);

                unsigned n = 0;
                while (n < table_size) {
                    if (inp_.at_end()) break;

                    unsigned num = decode_number(inp_, tables_.bd);
                    if (num < 16) {
                        table[n++] = static_cast <u8>(num);
                    } else if (num < 18) {
                        // 16: repeat previous 3-10 times (3 bits)
                        // 17: repeat previous 11-138 times (7 bits)
                        unsigned count;
                        if (num == 16) {
                            count = (inp_.get_bits() >> 13) + 3;
                            inp_.add_bits(3);
                        } else {
                            // num == 17
                            count = (inp_.get_bits() >> 9) + 11;
                            inp_.add_bits(7);
                        }
                        if (n == 0) return false; // Can't repeat at start
                        while (count-- > 0 && n < table_size) {
                            table[n] = table[n - 1];
                            n++;
                        }
                    } else {
                        // 18: zeros 3-10 times (3 bits)
                        // 19: zeros 11-138 times (7 bits)
                        unsigned count;
                        if (num == 18) {
                            count = (inp_.get_bits() >> 13) + 3;
                            inp_.add_bits(3);
                        } else {
                            // num == 19
                            count = (inp_.get_bits() >> 9) + 11;
                            inp_.add_bits(7);
                        }
                        while (count-- > 0 && n < table_size) {
                            table[n++] = 0;
                        }
                    }
                }

                // Build tables (order: LD, DD, LDD, RD)
                unsigned dc = extra_dist_ ? rar::DCX : rar::DCB;
                make_decode_tables(table.data(), tables_.ld, rar::NC);
                make_decode_tables(table.data() + rar::NC, tables_.dd, dc);
                make_decode_tables(table.data() + rar::NC + dc, tables_.ldd, rar::LDC);
                make_decode_tables(table.data() + rar::NC + dc + rar::LDC, tables_.rd, rar::RC);

                return true;
            }

            void insert_old_dist(size_t distance) {
                for (size_t i = old_dist_.size() - 1; i > 0; i--) {
                    old_dist_[i] = old_dist_[i - 1];
                }
                old_dist_[0] = distance;
            }

            unsigned copy_string(unsigned length, size_t distance, size_t max_out) {
                size_t src_ptr = unp_ptr_ - distance;
                unsigned copied = 0;
                while (length-- > 0 && copied < max_out) {
                    u8 value = window_[src_ptr++ & max_win_mask_];
                    window_[unp_ptr_++ & max_win_mask_] = value;
                    copied++;
                }
                return copied;
            }

            bool extra_dist_ = false;
            bool solid_mode_ = false; // Preserve window between files (solid archive)
            rar_bit_input inp_;
            std::vector <u8> window_;
            size_t max_win_size_ = 0;
            size_t max_win_mask_ = 0;
            size_t unp_ptr_ = 0;
            size_t wr_ptr_ = 0;
            bool first_win_done_ = false;
            bool tables_read_ = false;

            std::array <size_t, 4> old_dist_{};
            unsigned last_length_ = 0;

            BlockHeader block_header_;
            rar_block_tables tables_;

            // Filter processor for post-decompression transformations
            rar5_filter_processor filter_processor_;
            u64 written_file_pos_ = 0; // Track file position for filter application
    };
} // namespace crate
