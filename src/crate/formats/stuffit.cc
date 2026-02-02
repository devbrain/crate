// StuffIt archive format
// Based on deark by Jason Summers (MIT License)
// Compression algorithms based on XADMaster (LGPL v2.1)

#include <crate/formats/stuffit.hh>
#include <crate/core/crc.hh>
#include <crate/compression/inflate.hh>
#include <crate/compression/stuffit_rle.hh>
#include <crate/compression/stuffit_huffman.hh>
#include <crate/compression/stuffit_lzw.hh>
#include <cstring>
#include <algorithm>
#include <memory>
#include <array>
#include <vector>

namespace crate {
    namespace {
        // Signatures
        constexpr u8 SIG_OLD[] = {'S', 'I', 'T', '!'};
        constexpr u8 SIG_V5[] = {'S', 't', 'u', 'f', 'f', 'I', 't', ' '};

        // Old format member header size
        constexpr size_t OLD_MEMBER_HEADER_SIZE = 112;

        // Maximum nesting level for folders
        constexpr int MAX_NESTING_LEVEL = 32;

        // Read big-endian integers
        inline u16 read_u16be(const byte* p) {
            return static_cast <u16>((static_cast <u16>(p[0]) << 8) | p[1]);
        }

        inline u32 read_u32be(const byte* p) {
            return (static_cast <u32>(p[0]) << 24) |
                   (static_cast <u32>(p[1]) << 16) |
                   (static_cast <u32>(p[2]) << 8) |
                   static_cast <u32>(p[3]);
        }

        // Mac timestamp epoch: 1904-01-01
        constexpr i64 MAC_EPOCH_OFFSET = 2082844800;

        dos_date_time mac_time_to_dos(u32 mac_time) {
            if (mac_time == 0) return {};

            // Convert Mac time (seconds since 1904) to Unix time
            i64 unix_time = static_cast <i64>(mac_time) - MAC_EPOCH_OFFSET;
            if (unix_time < 0) return {};

            // Convert to DOS date/time (simplified)
            // This is approximate - proper conversion would use gmtime
            dos_date_time dt{};
            // For now, store raw values - proper implementation would convert
            return dt;
        }

        // ============================================================
        // Bit stream reader (LSB first)
        // ============================================================
        class bit_reader {
            public:
                bit_reader(byte_span data)
                    : data_(data), pos_(0), bits_(0), num_bits_(0) {
                }

                bool eof() const { return pos_ >= data_.size() && num_bits_ == 0; }

                u32 read_bits_le(int count) {
                    while (num_bits_ < count) {
                        if (pos_ >= data_.size()) return bits_;
                        bits_ |= static_cast <u32>(data_[pos_++]) << num_bits_;
                        num_bits_ += 8;
                    }
                    u32 result = bits_ & ((1u << count) - 1);
                    bits_ >>= count;
                    num_bits_ -= count;
                    return result;
                }

                u32 read_bits_be(int count) {
                    u32 result = 0;
                    for (int i = 0; i < count; i++) {
                        if (num_bits_ == 0) {
                            if (pos_ >= data_.size()) return result;
                            bits_ = data_[pos_++];
                            num_bits_ = 8;
                        }
                        result = (result << 1) | ((bits_ >> 7) & 1);
                        bits_ <<= 1;
                        num_bits_--;
                    }
                    return result;
                }

                u8 read_byte() {
                    align_to_byte();
                    if (pos_ >= data_.size()) return 0;
                    return data_[pos_++];
                }

                void align_to_byte() {
                    bits_ = 0;
                    num_bits_ = 0;
                }

                size_t position() const { return pos_; }

            private:
                byte_span data_;
                size_t pos_;
                u32 bits_;
                int num_bits_;
        };

        // ============================================================
        // Huffman prefix code table
        // ============================================================
        class huffman_table {
            public:
                static constexpr size_t MAX_SYMBOLS = 1024;

                huffman_table()
                    : tree_(MAX_SYMBOLS * 2), num_symbols_(0) {
                    std::fill(tree_.begin(), tree_.end(), 0);
                }

                void init_from_lengths(const int* lengths, size_t num_symbols, bool lsb_first = true) {
                    num_symbols_ = num_symbols;
                    next_free_ = 2;
                    std::fill(tree_.begin(), tree_.end(), 0);

                    // Count codes per length
                    std::array <size_t, 32> bl_count{};
                    size_t max_len = 0;
                    for (size_t i = 0; i < num_symbols; i++) {
                        if (lengths[i] > 0) {
                            bl_count[static_cast <size_t>(lengths[i])]++;
                            if (static_cast <size_t>(lengths[i]) > max_len) max_len = static_cast <size_t>(lengths[i]);
                        }
                    }

                    // Generate starting code for each length
                    std::array <u32, 32> next_code{};
                    u32 code = 0;
                    for (size_t bits = 1; bits <= max_len; bits++) {
                        code = (code + static_cast <u32>(bl_count[bits - 1])) << 1;
                        next_code[bits] = code;
                    }

                    // Assign codes to symbols
                    for (size_t i = 0; i < num_symbols; i++) {
                        int len = lengths[i];
                        if (len > 0) {
                            u32 c = next_code[static_cast <size_t>(len)]++;
                            // Reverse bits if LSB first
                            if (lsb_first) {
                                u32 rev = 0;
                                for (int b = 0; b < len; b++) {
                                    rev = (rev << 1) | ((c >> b) & 1);
                                }
                                c = rev;
                            }
                            add_code(c, len, static_cast <int>(i));
                        }
                    }
                }

                // Initialize from explicit codes and lengths (for meta-code)
                void init_from_explicit_codes(const int* codes, const int* lengths, size_t num_symbols) {
                    num_symbols_ = num_symbols;
                    next_free_ = 2;
                    std::fill(tree_.begin(), tree_.end(), 0);

                    for (size_t i = 0; i < num_symbols; i++) {
                        if (lengths[i] > 0) {
                            add_code(static_cast <u32>(codes[i]), lengths[i], static_cast <int>(i));
                        }
                    }
                }

                // Parse dynamic Huffman code lengths using meta-code
                // Algorithm from XADMaster/XADStuffIt13Handle.m
                bool parse_dynamic_code(bit_reader& br, const huffman_table& meta_code, size_t num_codes) {
                    std::vector <int> lengths(num_codes, 0);
                    int length = 0;

                    // The loop iterates through symbols, incrementing i at end of each iteration
                    // But repeat/conditional cases may increment i multiple times
                    for (size_t i = 0; i < num_codes; i++) {
                        int val = meta_code.decode_le(br);
                        if (val < 0) return false;

                        switch (val) {
                            case 31: // Set length to -1 (invalid/unused)
                                length = -1;
                                break;
                            case 32: // Increment length
                                length++;
                                break;
                            case 33: // Decrement length
                                length--;
                                break;
                            case 34: // Conditional: if next bit is 1, set current symbol and advance
                                if (br.read_bits_le(1)) {
                                    if (i < num_codes) lengths[i++] = length;
                                }
                                break;
                            case 35: {
                                // Repeat 2-9 times
                                int repeat = static_cast <int>(br.read_bits_le(3)) + 2;
                                while (repeat-- > 0 && i < num_codes) {
                                    lengths[i++] = length;
                                }
                                break;
                            }
                            case 36: {
                                // Repeat 10-73 times
                                int repeat = static_cast <int>(br.read_bits_le(6)) + 10;
                                while (repeat-- > 0 && i < num_codes) {
                                    lengths[i++] = length;
                                }
                                break;
                            }
                            default: // 0-30: Set length = val + 1
                                length = val + 1;
                                break;
                        }
                        // After processing, set current symbol to current length
                        // (unless we've already gone past the end)
                        if (i < num_codes) {
                            lengths[i] = length;
                        }
                    }

                    // Build the Huffman tree from parsed lengths
                    init_from_lengths(lengths.data(), num_codes, true);
                    return true;
                }

                int decode_le(bit_reader& br) const {
                    size_t node = 0;
                    while (node < num_symbols_ * 2) {
                        size_t bit = br.read_bits_le(1);
                        node = static_cast <size_t>(tree_[node + bit]);
                        if (node >= num_symbols_ * 2) {
                            return static_cast <int>(node - num_symbols_ * 2);
                        }
                        if (node == 0) return -1; // Invalid code
                    }
                    return -1;
                }

                int decode_be(bit_reader& br) const {
                    size_t node = 0;
                    while (node < num_symbols_ * 2) {
                        size_t bit = br.read_bits_be(1);
                        node = static_cast <size_t>(tree_[node + bit]);
                        if (node >= num_symbols_ * 2) {
                            return static_cast <int>(node - num_symbols_ * 2);
                        }
                        if (node == 0) return -1;
                    }
                    return -1;
                }

            private:
                void add_code(u32 code, int len, int symbol) {
                    size_t node = 0;
                    for (int i = 0; i < len - 1; i++) {
                        size_t bit = (code >> i) & 1;
                        if (tree_[node + bit] == 0) {
                            int new_node = next_free_;
                            next_free_ += 2;
                            tree_[node + bit] = new_node;
                        }
                        node = static_cast <size_t>(tree_[node + bit]);
                    }
                    size_t bit = (code >> (len - 1)) & 1;
                    tree_[node + bit] = static_cast <int>(num_symbols_) * 2 + symbol;
                }

                std::vector <int> tree_;
                size_t num_symbols_;
                int next_free_ = 2;
        };

        // ============================================================
        // Method 3: Simple Huffman
        // Binary tree encoded in bitstream, then data decoded using that tree
        // ============================================================
        class huffman_tree_decoder {
            public:
                bool build_from_stream(bit_reader& br) {
                    tree_.clear();
                    tree_.reserve(512);
                    return parse_tree_node(br);
                }

                int decode(bit_reader& br) const {
                    if (tree_.empty()) return -1;
                    size_t idx = 0;
                    while (idx < tree_.size()) {
                        if (tree_[idx].is_leaf) {
                            return tree_[idx].value;
                        }
                        u32 bit = br.read_bits_be(1);
                        idx = bit ? tree_[idx].one_child : tree_[idx].zero_child;
                    }
                    return -1;
                }

            private:
                struct tree_node {
                    bool is_leaf = false;
                    int value = 0;
                    size_t zero_child = 0;
                    size_t one_child = 0;
                };

                bool parse_tree_node(bit_reader& br) {
                    size_t idx = tree_.size();
                    tree_.push_back({});

                    if (br.read_bits_be(1) == 1) {
                        // Leaf node
                        tree_[idx].is_leaf = true;
                        tree_[idx].value = static_cast <int>(br.read_bits_be(8));
                    } else {
                        // Internal node
                        tree_[idx].zero_child = tree_.size();
                        if (!parse_tree_node(br)) return false;
                        tree_[idx].one_child = tree_.size();
                        if (!parse_tree_node(br)) return false;
                    }
                    return true;
                }

                std::vector <tree_node> tree_;
        };

        // ============================================================
        // Method 13: LZSS + Huffman (StuffIt 13)
        // Based on XADStuffIt13Handle
        // ============================================================

        // Static Huffman code length tables for method 13 (from XADMaster)
        static const int FirstCodeLengths_1[321] = {
            4, 5, 7, 8, 8, 9, 9, 9, 9, 7, 9, 9, 9, 8, 9, 9,
            9, 9, 9, 9, 9, 9, 9, 10, 9, 9, 10, 10, 9, 10, 9, 9,
            5, 9, 9, 9, 9, 10, 9, 9, 9, 9, 9, 9, 9, 9, 7, 9,
            9, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
            9, 8, 9, 9, 8, 8, 9, 9, 9, 9, 9, 9, 9, 7, 8, 9,
            7, 9, 9, 7, 7, 9, 9, 9, 9, 10, 9, 10, 10, 10, 9, 9,
            9, 5, 9, 8, 7, 5, 9, 8, 8, 7, 9, 9, 8, 8, 5, 5,
            7, 10, 5, 8, 5, 8, 9, 9, 9, 9, 9, 10, 9, 9, 10, 9,
            9, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 10, 10, 10,
            9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
            9, 10, 10, 10, 10, 10, 10, 10, 9, 9, 10, 10, 10, 10, 10, 10,
            10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 10,
            9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
            10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
            9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 9, 10, 10,
            9, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 9, 10, 9, 5,
            6, 5, 5, 8, 9, 9, 9, 9, 9, 9, 10, 10, 10, 9, 10, 10,
            10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
            10, 10, 10, 9, 10, 9, 9, 9, 10, 9, 10, 9, 10, 9, 10, 9,
            10, 10, 10, 9, 10, 9, 10, 10, 9, 9, 9, 6, 9, 9, 10, 9,
            5,
        };
        static const int SecondCodeLengths_1[321] = {
            4, 5, 6, 6, 7, 7, 6, 7, 7, 7, 6, 8, 7, 8, 8, 8,
            8, 9, 6, 9, 8, 9, 8, 9, 9, 9, 8, 10, 5, 9, 7, 9,
            6, 9, 8, 10, 9, 10, 8, 8, 9, 9, 7, 9, 8, 9, 8, 9,
            8, 8, 6, 9, 9, 8, 8, 9, 9, 10, 8, 9, 9, 10, 8, 10,
            8, 8, 8, 8, 8, 9, 7, 10, 6, 9, 9, 11, 7, 8, 8, 9,
            8, 10, 7, 8, 6, 9, 10, 9, 9, 10, 8, 11, 9, 11, 9, 10,
            9, 8, 9, 8, 8, 8, 8, 10, 9, 9, 10, 10, 8, 9, 8, 8,
            8, 11, 9, 8, 8, 9, 9, 10, 8, 11, 10, 10, 8, 10, 9, 10,
            8, 9, 9, 11, 9, 11, 9, 10, 10, 11, 10, 12, 9, 12, 10, 11,
            10, 11, 9, 10, 10, 11, 10, 11, 10, 11, 10, 11, 10, 10, 10, 9,
            9, 9, 8, 7, 6, 8, 11, 11, 9, 12, 10, 12, 9, 11, 11, 11,
            10, 12, 11, 11, 10, 12, 10, 11, 10, 10, 10, 11, 10, 11, 11, 11,
            9, 12, 10, 12, 11, 12, 10, 11, 10, 12, 11, 12, 11, 12, 11, 12,
            10, 12, 11, 12, 11, 11, 10, 12, 10, 11, 10, 12, 10, 12, 10, 12,
            10, 11, 11, 11, 10, 11, 11, 11, 10, 12, 11, 12, 10, 10, 11, 11,
            9, 12, 11, 12, 10, 11, 10, 12, 10, 11, 10, 12, 10, 11, 10, 7,
            5, 4, 6, 6, 7, 7, 7, 8, 8, 7, 7, 6, 8, 6, 7, 7,
            9, 8, 9, 9, 10, 11, 11, 11, 12, 11, 10, 11, 12, 11, 12, 11,
            12, 12, 12, 12, 11, 12, 12, 11, 12, 11, 12, 11, 13, 11, 12, 10,
            13, 10, 14, 14, 13, 14, 15, 14, 16, 15, 15, 18, 18, 18, 9, 18,
            8,
        };
        static const int OffsetCodeLengths_1[11] = {5, 6, 3, 3, 3, 3, 3, 3, 3, 4, 6};

        static const int FirstCodeLengths_2[321] = {
            4, 7, 7, 8, 7, 8, 8, 8, 8, 7, 8, 7, 8, 7, 9, 8,
            8, 8, 9, 9, 9, 9, 10, 10, 9, 10, 10, 10, 10, 10, 9, 9,
            5, 9, 8, 9, 9, 11, 10, 9, 8, 9, 9, 9, 8, 9, 7, 8,
            8, 8, 9, 9, 9, 9, 9, 10, 9, 9, 9, 10, 9, 9, 10, 9,
            8, 8, 7, 7, 7, 8, 8, 9, 8, 8, 9, 9, 8, 8, 7, 8,
            7, 10, 8, 7, 7, 9, 9, 9, 9, 10, 10, 11, 11, 11, 10, 9,
            8, 6, 8, 7, 7, 5, 7, 7, 7, 6, 9, 8, 6, 7, 6, 6,
            7, 9, 6, 6, 6, 7, 8, 8, 8, 8, 9, 10, 9, 10, 9, 9,
            8, 9, 10, 10, 9, 10, 10, 9, 9, 10, 10, 10, 10, 10, 10, 10,
            9, 10, 10, 11, 10, 10, 10, 10, 10, 10, 10, 11, 10, 11, 10, 10,
            9, 11, 10, 10, 10, 10, 10, 10, 9, 9, 10, 11, 10, 11, 10, 11,
            10, 12, 10, 11, 10, 12, 11, 12, 10, 12, 10, 11, 10, 11, 11, 11,
            9, 10, 11, 11, 11, 12, 12, 10, 10, 10, 11, 11, 10, 11, 10, 10,
            9, 11, 10, 11, 10, 11, 11, 11, 10, 11, 11, 12, 11, 11, 10, 10,
            10, 11, 10, 10, 11, 11, 12, 10, 10, 11, 11, 12, 11, 11, 10, 11,
            9, 12, 10, 11, 11, 11, 10, 11, 10, 11, 10, 11, 9, 10, 9, 7,
            3, 5, 6, 6, 7, 7, 8, 8, 8, 9, 9, 9, 11, 10, 10, 10,
            12, 13, 11, 12, 12, 11, 13, 12, 12, 11, 12, 12, 13, 12, 14, 13,
            14, 13, 15, 13, 14, 15, 15, 14, 13, 15, 15, 14, 15, 14, 15, 15,
            14, 15, 13, 13, 14, 15, 15, 14, 14, 16, 16, 15, 15, 15, 12, 15,
            10,
        };
        static const int SecondCodeLengths_2[321] = {
            5, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 8, 7, 8, 7, 7,
            7, 8, 8, 8, 8, 9, 8, 9, 8, 9, 9, 9, 7, 9, 8, 8,
            6, 9, 8, 9, 8, 9, 8, 9, 8, 9, 8, 9, 8, 9, 8, 8,
            8, 8, 8, 9, 8, 9, 8, 9, 9, 10, 8, 10, 8, 9, 9, 8,
            8, 8, 7, 8, 8, 9, 8, 9, 7, 9, 8, 10, 8, 9, 8, 9,
            8, 9, 8, 8, 8, 9, 9, 9, 9, 10, 9, 11, 9, 10, 9, 10,
            8, 8, 8, 9, 8, 8, 8, 9, 9, 8, 9, 10, 8, 9, 8, 8,
            8, 11, 8, 7, 8, 9, 9, 9, 9, 10, 9, 10, 9, 10, 9, 8,
            8, 9, 9, 10, 9, 10, 9, 10, 8, 10, 9, 10, 9, 11, 10, 11,
            9, 11, 10, 10, 10, 11, 9, 11, 9, 10, 9, 11, 9, 11, 10, 10,
            9, 10, 9, 9, 8, 10, 9, 11, 9, 9, 9, 11, 10, 11, 9, 11,
            9, 11, 9, 11, 10, 11, 10, 11, 10, 11, 9, 10, 10, 11, 10, 10,
            8, 10, 9, 10, 10, 11, 9, 11, 9, 10, 10, 11, 9, 10, 10, 9,
            9, 10, 9, 10, 9, 10, 9, 10, 9, 11, 9, 11, 10, 10, 9, 10,
            9, 11, 9, 11, 9, 11, 9, 10, 9, 11, 9, 11, 9, 11, 9, 10,
            8, 11, 9, 10, 9, 10, 9, 10, 8, 10, 8, 9, 8, 9, 8, 7,
            4, 4, 5, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 7, 8, 8,
            9, 9, 10, 10, 10, 10, 10, 10, 11, 11, 10, 10, 12, 11, 11, 12,
            12, 11, 12, 12, 11, 12, 12, 12, 12, 12, 12, 11, 12, 11, 13, 12,
            13, 12, 13, 14, 14, 14, 15, 13, 14, 13, 14, 18, 18, 17, 7, 16,
            9,
        };
        static const int OffsetCodeLengths_2[13] = {5, 6, 4, 4, 3, 3, 3, 3, 3, 4, 4, 4, 6};

        static const int FirstCodeLengths_3[321] = {
            6, 6, 6, 6, 6, 9, 8, 8, 4, 9, 8, 9, 8, 9, 9, 9,
            8, 9, 9, 10, 8, 10, 10, 10, 9, 10, 10, 10, 9, 10, 10, 9,
            9, 9, 8, 10, 9, 10, 9, 10, 9, 10, 9, 10, 9, 9, 8, 9,
            8, 9, 9, 9, 10, 10, 10, 10, 9, 9, 9, 10, 9, 10, 9, 9,
            7, 8, 8, 9, 8, 9, 9, 9, 8, 9, 9, 10, 9, 9, 8, 9,
            8, 9, 8, 8, 8, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 9,
            8, 8, 9, 8, 9, 7, 8, 8, 9, 8, 10, 10, 8, 9, 8, 8,
            8, 10, 8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 10, 9,
            7, 9, 9, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 10, 10, 9,
            9, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 10, 10,
            9, 10, 10, 10, 10, 10, 10, 10, 9, 9, 9, 10, 10, 10, 10, 10,
            10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 9,
            8, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10,
            9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9,
            9, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 10, 10, 9, 9,
            9, 10, 10, 10, 10, 10, 10, 9, 9, 10, 9, 9, 8, 9, 8, 9,
            4, 6, 6, 6, 7, 8, 8, 9, 9, 10, 10, 10, 9, 10, 10, 10,
            10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 7, 10,
            10, 10, 7, 10, 10, 7, 7, 7, 7, 7, 6, 7, 10, 7, 7, 10,
            7, 7, 7, 6, 7, 6, 6, 7, 7, 6, 6, 9, 6, 9, 10, 6,
            10,
        };
        static const int SecondCodeLengths_3[321] = {
            5, 6, 6, 6, 6, 7, 7, 7, 6, 8, 7, 8, 7, 9, 8, 8,
            7, 7, 8, 9, 9, 9, 9, 10, 8, 9, 9, 10, 8, 10, 9, 8,
            6, 10, 8, 10, 8, 10, 9, 9, 9, 9, 9, 10, 9, 9, 8, 9,
            8, 9, 8, 9, 9, 10, 9, 10, 9, 9, 8, 10, 9, 11, 10, 8,
            8, 8, 8, 9, 7, 9, 9, 10, 8, 9, 8, 11, 9, 10, 9, 10,
            8, 9, 9, 9, 9, 8, 9, 9, 10, 10, 10, 12, 10, 11, 10, 10,
            8, 9, 9, 9, 8, 9, 8, 8, 10, 9, 10, 11, 8, 10, 9, 9,
            8, 12, 8, 9, 9, 9, 9, 8, 9, 10, 9, 12, 10, 10, 10, 8,
            7, 11, 10, 9, 10, 11, 9, 11, 7, 11, 10, 12, 10, 12, 10, 11,
            9, 11, 9, 12, 10, 12, 10, 12, 10, 9, 11, 12, 10, 12, 10, 11,
            9, 10, 9, 10, 9, 11, 11, 12, 9, 10, 8, 12, 11, 12, 9, 12,
            10, 12, 10, 13, 10, 12, 10, 12, 10, 12, 10, 9, 10, 12, 10, 9,
            8, 11, 10, 12, 10, 12, 10, 12, 10, 11, 10, 12, 8, 12, 10, 11,
            10, 10, 10, 12, 9, 11, 10, 12, 10, 12, 11, 12, 10, 9, 10, 12,
            9, 10, 10, 12, 10, 11, 10, 11, 10, 12, 8, 12, 9, 12, 8, 12,
            8, 11, 10, 11, 10, 11, 9, 10, 8, 10, 9, 9, 8, 9, 8, 7,
            4, 3, 5, 5, 6, 5, 6, 6, 7, 7, 8, 8, 8, 7, 7, 7,
            9, 8, 9, 9, 11, 9, 11, 9, 8, 9, 9, 11, 12, 11, 12, 12,
            13, 13, 12, 13, 14, 13, 14, 13, 14, 13, 13, 13, 12, 13, 13, 12,
            13, 13, 14, 14, 13, 13, 14, 14, 14, 14, 15, 18, 17, 18, 8, 16,
            10,
        };
        static const int OffsetCodeLengths_3[14] = {6, 7, 4, 4, 3, 3, 3, 3, 3, 4, 4, 4, 5, 7};

        static const int FirstCodeLengths_4[321] = {
            2, 6, 6, 7, 7, 8, 7, 8, 7, 8, 8, 9, 8, 9, 9, 9,
            8, 8, 9, 9, 9, 10, 10, 9, 8, 10, 9, 10, 9, 10, 9, 9,
            6, 9, 8, 9, 9, 10, 9, 9, 9, 10, 9, 9, 9, 9, 8, 8,
            8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 10, 10, 9,
            7, 7, 8, 8, 8, 8, 9, 9, 7, 8, 9, 10, 8, 8, 7, 8,
            8, 10, 8, 8, 8, 9, 8, 9, 9, 10, 9, 11, 10, 11, 9, 9,
            8, 7, 9, 8, 8, 6, 8, 8, 8, 7, 10, 9, 7, 8, 7, 7,
            8, 10, 7, 7, 7, 8, 9, 9, 9, 9, 10, 11, 9, 11, 10, 9,
            7, 9, 10, 10, 10, 11, 11, 10, 10, 11, 10, 10, 10, 11, 11, 10,
            9, 10, 10, 11, 10, 11, 10, 11, 10, 10, 10, 11, 10, 11, 10, 10,
            9, 10, 10, 11, 10, 10, 10, 10, 9, 10, 10, 10, 10, 11, 10, 11,
            10, 11, 10, 11, 11, 11, 10, 12, 10, 11, 10, 11, 10, 11, 11, 10,
            8, 10, 10, 11, 10, 11, 11, 11, 10, 11, 10, 11, 10, 11, 11, 11,
            9, 10, 11, 11, 10, 11, 11, 11, 10, 11, 11, 11, 10, 10, 10, 10,
            10, 11, 10, 10, 11, 11, 10, 10, 9, 11, 10, 10, 11, 11, 10, 10,
            10, 11, 10, 10, 10, 10, 10, 10, 9, 11, 10, 10, 8, 10, 8, 6,
            5, 6, 6, 7, 7, 8, 8, 8, 9, 10, 11, 10, 10, 11, 11, 12,
            12, 10, 11, 12, 12, 12, 12, 13, 13, 13, 13, 13, 12, 13, 13, 15,
            14, 12, 14, 15, 16, 12, 12, 13, 15, 14, 16, 15, 17, 18, 15, 17,
            16, 15, 15, 15, 15, 13, 13, 10, 14, 12, 13, 17, 17, 18, 10, 17,
            4,
        };
        static const int SecondCodeLengths_4[321] = {
            4, 5, 6, 6, 6, 6, 7, 7, 6, 7, 7, 9, 6, 8, 8, 7,
            7, 8, 8, 8, 6, 9, 8, 8, 7, 9, 8, 9, 8, 9, 8, 9,
            6, 9, 8, 9, 8, 10, 9, 9, 8, 10, 8, 10, 8, 9, 8, 9,
            8, 8, 7, 9, 9, 9, 9, 9, 8, 10, 9, 10, 9, 10, 9, 8,
            7, 8, 9, 9, 8, 9, 9, 9, 7, 10, 9, 10, 9, 9, 8, 9,
            8, 9, 8, 8, 8, 9, 9, 10, 9, 9, 8, 11, 9, 11, 10, 10,
            8, 8, 10, 8, 8, 9, 9, 9, 10, 9, 10, 11, 9, 9, 9, 9,
            8, 9, 8, 8, 8, 10, 10, 9, 9, 8, 10, 11, 10, 11, 11, 9,
            8, 9, 10, 11, 9, 10, 11, 11, 9, 12, 10, 10, 10, 12, 11, 11,
            9, 11, 11, 12, 9, 11, 9, 10, 10, 10, 10, 12, 9, 11, 10, 11,
            9, 11, 11, 11, 10, 11, 11, 12, 9, 10, 10, 12, 11, 11, 10, 11,
            9, 11, 10, 11, 10, 11, 9, 11, 11, 9, 8, 11, 10, 11, 11, 10,
            7, 12, 11, 11, 11, 11, 11, 12, 10, 12, 11, 13, 11, 10, 12, 11,
            10, 11, 10, 11, 10, 11, 11, 11, 10, 12, 11, 11, 10, 11, 10, 10,
            10, 11, 10, 12, 11, 12, 10, 11, 9, 11, 10, 11, 10, 11, 10, 12,
            9, 11, 11, 11, 9, 11, 10, 10, 9, 11, 10, 10, 9, 10, 9, 7,
            4, 5, 5, 5, 6, 6, 7, 6, 8, 7, 8, 9, 9, 7, 8, 8,
            10, 9, 10, 10, 12, 10, 11, 11, 11, 11, 10, 11, 12, 11, 11, 11,
            11, 11, 13, 12, 11, 12, 13, 12, 12, 12, 13, 11, 9, 12, 13, 7,
            13, 11, 13, 11, 10, 11, 13, 15, 15, 12, 14, 15, 15, 15, 6, 15,
            5,
        };
        static const int OffsetCodeLengths_4[11] = {3, 6, 5, 4, 2, 3, 3, 3, 4, 4, 6};

        static const int FirstCodeLengths_5[321] = {
            7, 9, 9, 9, 9, 9, 9, 9, 9, 8, 9, 9, 9, 7, 9, 9,
            9, 9, 9, 9, 9, 9, 9, 10, 9, 10, 9, 10, 9, 10, 9, 9,
            5, 9, 7, 9, 9, 9, 9, 9, 7, 7, 7, 9, 7, 7, 8, 7,
            8, 8, 7, 7, 9, 9, 9, 9, 7, 7, 7, 9, 9, 9, 9, 9,
            9, 7, 9, 7, 7, 7, 7, 9, 9, 7, 9, 9, 7, 7, 7, 7,
            7, 9, 7, 8, 7, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
            9, 7, 8, 7, 7, 7, 8, 8, 6, 7, 9, 7, 7, 8, 7, 5,
            6, 9, 5, 7, 5, 6, 7, 7, 9, 8, 9, 9, 9, 9, 9, 9,
            9, 9, 10, 9, 10, 10, 10, 9, 9, 10, 10, 10, 10, 10, 10, 10,
            9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10,
            9, 10, 10, 10, 9, 9, 10, 9, 9, 9, 9, 10, 10, 10, 10, 10,
            10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10,
            9, 10, 10, 10, 9, 10, 10, 10, 9, 9, 9, 10, 10, 10, 10, 10,
            9, 10, 9, 10, 10, 9, 10, 10, 9, 10, 10, 10, 10, 10, 10, 10,
            9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
            9, 10, 10, 10, 10, 10, 10, 10, 9, 10, 9, 10, 9, 10, 10, 9,
            5, 6, 8, 8, 7, 7, 7, 9, 9, 9, 9, 9, 9, 9, 9, 9,
            9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
            9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
            10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 5, 10, 8, 9, 8,
            9,
        };
        static const int SecondCodeLengths_5[321] = {
            8, 10, 11, 11, 11, 12, 11, 11, 12, 6, 11, 12, 10, 5, 12, 12,
            12, 12, 12, 12, 12, 13, 13, 14, 13, 13, 12, 13, 12, 13, 12, 15,
            4, 10, 7, 9, 11, 11, 10, 9, 6, 7, 8, 9, 6, 7, 6, 7,
            8, 7, 7, 8, 8, 8, 8, 8, 8, 9, 8, 7, 10, 9, 10, 10,
            11, 7, 8, 6, 7, 8, 8, 9, 8, 7, 10, 10, 8, 7, 8, 8,
            7, 10, 7, 6, 7, 9, 9, 8, 11, 11, 11, 10, 11, 11, 11, 8,
            11, 6, 7, 6, 6, 6, 6, 8, 7, 6, 10, 9, 6, 7, 6, 6,
            7, 10, 6, 5, 6, 7, 7, 7, 10, 8, 11, 9, 13, 7, 14, 16,
            12, 14, 14, 15, 15, 16, 16, 14, 15, 15, 15, 15, 15, 15, 15, 15,
            14, 15, 13, 14, 14, 16, 15, 17, 14, 17, 15, 17, 12, 14, 13, 16,
            12, 17, 13, 17, 14, 13, 13, 14, 14, 12, 13, 15, 15, 14, 15, 17,
            14, 17, 15, 14, 15, 16, 12, 16, 15, 14, 15, 16, 15, 16, 17, 17,
            15, 15, 17, 17, 13, 14, 15, 15, 13, 12, 16, 16, 17, 14, 15, 16,
            15, 15, 13, 13, 15, 13, 16, 17, 15, 17, 17, 17, 16, 17, 14, 17,
            14, 16, 15, 17, 15, 15, 14, 17, 15, 17, 15, 16, 15, 15, 16, 16,
            14, 17, 17, 15, 15, 16, 15, 17, 15, 14, 16, 16, 16, 16, 16, 12,
            4, 4, 5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 8, 8, 9, 9,
            9, 9, 9, 10, 10, 10, 11, 10, 11, 11, 11, 11, 11, 12, 12, 12,
            13, 13, 12, 13, 12, 14, 14, 12, 13, 13, 13, 13, 14, 12, 13, 13,
            14, 14, 14, 13, 14, 14, 15, 15, 13, 15, 13, 17, 17, 17, 9, 17,
            7,
        };
        static const int OffsetCodeLengths_5[11] = {6, 7, 7, 6, 4, 3, 2, 2, 3, 3, 6};

        static const int* FirstCodeLengths[5] = {
            FirstCodeLengths_1, FirstCodeLengths_2, FirstCodeLengths_3,
            FirstCodeLengths_4, FirstCodeLengths_5
        };
        static const int* SecondCodeLengths[5] = {
            SecondCodeLengths_1, SecondCodeLengths_2, SecondCodeLengths_3,
            SecondCodeLengths_4, SecondCodeLengths_5
        };
        static const int* OffsetCodeLengths[5] = {
            OffsetCodeLengths_1, OffsetCodeLengths_2, OffsetCodeLengths_3,
            OffsetCodeLengths_4, OffsetCodeLengths_5
        };
        static const int OffsetCodeSize[5] = {11, 13, 14, 11, 11};

        // Meta codes for dynamic table building
        static const int MetaCodes[37] = {
            0x5d8, 0x058, 0x040, 0x0c0, 0x000, 0x078, 0x02b, 0x014,
            0x00c, 0x01c, 0x01b, 0x00b, 0x010, 0x020, 0x038, 0x018,
            0x0d8, 0xbd8, 0x180, 0x680, 0x380, 0xf80, 0x780, 0x480,
            0x080, 0x280, 0x3d8, 0xfd8, 0x7d8, 0x9d8, 0x1d8, 0x004,
            0x001, 0x002, 0x007, 0x003, 0x008
        };

        static const int MetaCodeLengths[37] = {
            11, 8, 8, 8, 8, 7, 6, 5, 5, 5, 5, 6, 5, 6, 7, 7, 9, 12, 10, 11, 11, 12,
            12, 11, 11, 11, 12, 12, 12, 12, 12, 5, 2, 2, 3, 4, 5
        };

        result_t <size_t> decompress_method13(byte_span input, mutable_byte_span output) {
            bit_reader br(input);

            // Read code type
            u8 val = br.read_byte();
            int code_type = val >> 4;

            huffman_table first_code, second_code, offset_code;

            if (code_type == 0) {
                // Dynamic tables - parse Huffman codes using meta-code
                huffman_table meta_code;
                meta_code.init_from_explicit_codes(MetaCodes, MetaCodeLengths, 37);

                // Parse first code (321 symbols for literal/length)
                if (!first_code.parse_dynamic_code(br, meta_code, 321)) {
                    return std::unexpected(error{
                        error_code::CorruptData,
                        "Failed to parse method 13 dynamic first code"
                    });
                }

                // Parse second code - either same as first (val & 0x08) or parse new
                if (val & 0x08) {
                    // Reuse first code as second code
                    second_code = first_code;
                } else {
                    if (!second_code.parse_dynamic_code(br, meta_code, 321)) {
                        return std::unexpected(error{
                            error_code::CorruptData,
                            "Failed to parse method 13 dynamic second code"
                        });
                    }
                }

                // Parse offset code with (val & 0x07) + 10 symbols
                size_t offset_size = static_cast <size_t>((val & 0x07) + 10);
                if (!offset_code.parse_dynamic_code(br, meta_code, offset_size)) {
                    return std::unexpected(error{
                        error_code::CorruptData,
                        "Failed to parse method 13 dynamic offset code"
                    });
                }
            } else if (code_type >= 1 && code_type <= 5) {
                // Use predefined tables
                size_t table_idx = static_cast <size_t>(code_type - 1);
                first_code.init_from_lengths(FirstCodeLengths[table_idx], 321, true);
                second_code.init_from_lengths(SecondCodeLengths[table_idx], 321, true);
                offset_code.init_from_lengths(OffsetCodeLengths[table_idx],
                                              static_cast <size_t>(OffsetCodeSize[table_idx]), true);
            } else {
                return std::unexpected(error{
                    error_code::CorruptData,
                    "Invalid method 13 code type"
                });
            }

            // LZSS window
            constexpr size_t WINDOW_SIZE = 65536;
            std::vector <byte> window(WINDOW_SIZE, 0);
            size_t window_pos = 0;

            byte* out = output.data();
            byte* out_end = output.data() + output.size();

            huffman_table* curr_code = &first_code;

            while (out < out_end && !br.eof()) {
                int sym = curr_code->decode_le(br);
                if (sym < 0) break;

                if (sym < 0x100) {
                    // Literal byte
                    *out++ = static_cast <byte>(sym);
                    window[window_pos++ % WINDOW_SIZE] = static_cast <byte>(sym);
                    curr_code = &first_code;
                } else if (sym < 0x140) {
                    // Match
                    curr_code = &second_code;

                    size_t length;
                    if (sym < 0x13e) {
                        length = static_cast <size_t>(sym - 0x100 + 3);
                    } else if (sym == 0x13e) {
                        length = br.read_bits_le(10) + 65;
                    } else if (sym == 0x13f) {
                        length = br.read_bits_le(15) + 65;
                    } else {
                        break; // End marker (0x140)
                    }

                    int bit_len = offset_code.decode_le(br);
                    if (bit_len < 0) break;

                    size_t offset;
                    if (bit_len == 0) offset = 1;
                    else if (bit_len == 1) offset = 2;
                    else offset = (1u << (bit_len - 1)) + br.read_bits_le(bit_len - 1) + 1;

                    // Copy from window
                    for (size_t i = 0; i < length && out < out_end; i++) {
                        size_t src_pos = (window_pos - offset + WINDOW_SIZE) % WINDOW_SIZE;
                        byte b = window[src_pos];
                        *out++ = b;
                        window[window_pos++ % WINDOW_SIZE] = b;
                    }
                } else {
                    // End of stream
                    break;
                }
            }

            return static_cast <size_t>(out - output.data());
        }

        // ============================================================
        // Method 15: Arsenic (BWT + Arithmetic coding + MTF)
        // Based on XADStuffItArsenicHandle
        // ============================================================

        // Arithmetic decoder model
        struct arsenic_symbol {
            int symbol;
            int frequency;
        };

        struct arsenic_model {
            int total_frequency;
            int increment;
            int frequency_limit;
            size_t num_symbols;
            std::array <arsenic_symbol, 128> symbols;

            void init(int first_sym, int last_sym, int inc, int freq_limit) {
                increment = inc;
                frequency_limit = freq_limit;
                num_symbols = static_cast <size_t>(last_sym - first_sym + 1);
                for (size_t i = 0; i < num_symbols; i++) {
                    symbols[i].symbol = static_cast <int>(i) + first_sym;
                    symbols[i].frequency = increment;
                }
                total_frequency = increment * static_cast <int>(num_symbols);
            }

            void reset() {
                total_frequency = increment * static_cast <int>(num_symbols);
                for (size_t i = 0; i < num_symbols; i++) {
                    symbols[i].frequency = increment;
                }
            }

            void increase_frequency(size_t idx) {
                symbols[idx].frequency += increment;
                total_frequency += increment;

                if (total_frequency > frequency_limit) {
                    total_frequency = 0;
                    for (size_t i = 0; i < num_symbols; i++) {
                        symbols[i].frequency++;
                        symbols[i].frequency >>= 1;
                        total_frequency += symbols[i].frequency;
                    }
                }
            }
        };

        // Arithmetic decoder
        class arsenic_decoder {
            public:
                static constexpr i32 NUM_BITS = 26;
                static constexpr i32 ONE = 1 << (NUM_BITS - 1);
                static constexpr i32 HALF = 1 << (NUM_BITS - 2);

                void init(bit_reader& br) {
                    br_ = &br;
                    range_ = ONE;
                    code_ = 0;
                    for (int i = 0; i < NUM_BITS; i++) {
                        code_ = (code_ << 1) | static_cast <i32>(br_->read_bits_be(1));
                    }
                }

                int next_symbol(arsenic_model& model) {
                    i32 freq = code_ / (range_ / model.total_frequency);
                    i32 cumulative = 0;
                    size_t n = 0;
                    for (n = 0; n < model.num_symbols - 1; n++) {
                        if (cumulative + model.symbols[n].frequency > freq) break;
                        cumulative += model.symbols[n].frequency;
                    }

                    read_next_code(static_cast <i32>(cumulative), model.symbols[n].frequency, model.total_frequency);
                    model.increase_frequency(n);

                    return model.symbols[n].symbol;
                }

                int next_bit_string(arsenic_model& model, int bits) {
                    int result = 0;
                    for (int i = 0; i < bits; i++) {
                        if (next_symbol(model)) result |= (1 << i);
                    }
                    return result;
                }

            private:
                void read_next_code(i32 sym_low, i32 sym_size, i32 sym_tot) {
                    i32 renorm_factor = range_ / sym_tot;
                    i32 low_incr = renorm_factor * sym_low;

                    code_ -= low_incr;
                    if (sym_low + sym_size == sym_tot) {
                        range_ -= low_incr;
                    } else {
                        range_ = sym_size * renorm_factor;
                    }

                    while (range_ <= HALF) {
                        range_ <<= 1;
                        code_ = (code_ << 1) | static_cast <i32>(br_->read_bits_be(1));
                    }
                }

                bit_reader* br_ = nullptr;
                i32 range_ = 0;
                i32 code_ = 0;
        };

        // Move-to-Front decoder
        class mtf_decoder {
            public:
                mtf_decoder() {
                    for (size_t i = 0; i < 256; i++) {
                        table_[i] = static_cast <u8>(i);
                    }
                }

                void reset() {
                    for (size_t i = 0; i < 256; i++) {
                        table_[i] = static_cast <u8>(i);
                    }
                }

                u8 decode(size_t symbol) {
                    u8 val = table_[symbol];
                    // Move to front
                    for (size_t i = symbol; i > 0; i--) {
                        table_[i] = table_[i - 1];
                    }
                    table_[0] = val;
                    return val;
                }

            private:
                std::array <u8, 256> table_;
        };

        // Randomization table for Arsenic
        static const u16 RandomizationTable[256] = {
            0xee, 0x56, 0xf8, 0xc3, 0x9d, 0x9f, 0xae, 0x2c,
            0xad, 0xcd, 0x24, 0x9d, 0xa6, 0x101, 0x18, 0xb9,
            0xa1, 0x82, 0x75, 0xe9, 0x9f, 0x55, 0x66, 0x6a,
            0x86, 0x71, 0xdc, 0x84, 0x56, 0x96, 0x56, 0xa1,
            0x84, 0x78, 0xb7, 0x32, 0x6a, 0x3, 0xe3, 0x2,
            0x11, 0x101, 0x8, 0x44, 0x83, 0x100, 0x43, 0xe3,
            0x1c, 0xf0, 0x86, 0x6a, 0x6b, 0xf, 0x3, 0x2d,
            0x86, 0x17, 0x7b, 0x10, 0xf6, 0x80, 0x78, 0x7a,
            0xa1, 0xe1, 0xef, 0x8c, 0xf6, 0x87, 0x4b, 0xa7,
            0xe2, 0x77, 0xfa, 0xb8, 0x81, 0xee, 0x77, 0xc0,
            0x9d, 0x29, 0x20, 0x27, 0x71, 0x12, 0xe0, 0x6b,
            0xd1, 0x7c, 0xa, 0x89, 0x7d, 0x87, 0xc4, 0x101,
            0xc1, 0x31, 0xaf, 0x38, 0x3, 0x68, 0x1b, 0x76,
            0x79, 0x3f, 0xdb, 0xc7, 0x1b, 0x36, 0x7b, 0xe2,
            0x63, 0x81, 0xee, 0xc, 0x63, 0x8b, 0x78, 0x38,
            0x97, 0x9b, 0xd7, 0x8f, 0xdd, 0xf2, 0xa3, 0x77,
            0x8c, 0xc3, 0x39, 0x20, 0xb3, 0x12, 0x11, 0xe,
            0x17, 0x42, 0x80, 0x2c, 0xc4, 0x92, 0x59, 0xc8,
            0xdb, 0x40, 0x76, 0x64, 0xb4, 0x55, 0x1a, 0x9e,
            0xfe, 0x5f, 0x6, 0x3c, 0x41, 0xef, 0xd4, 0xaa,
            0x98, 0x29, 0xcd, 0x1f, 0x2, 0xa8, 0x87, 0xd2,
            0xa0, 0x93, 0x98, 0xef, 0xc, 0x43, 0xed, 0x9d,
            0xc2, 0xeb, 0x81, 0xe9, 0x64, 0x23, 0x68, 0x1e,
            0x25, 0x57, 0xde, 0x9a, 0xcf, 0x7f, 0xe5, 0xba,
            0x41, 0xea, 0xea, 0x36, 0x1a, 0x28, 0x79, 0x20,
            0x5e, 0x18, 0x4e, 0x7c, 0x8e, 0x58, 0x7a, 0xef,
            0x91, 0x2, 0x93, 0xbb, 0x56, 0xa1, 0x49, 0x1b,
            0x79, 0x92, 0xf3, 0x58, 0x4f, 0x52, 0x9c, 0x2,
            0x77, 0xaf, 0x2a, 0x8f, 0x49, 0xd0, 0x99, 0x4d,
            0x98, 0x101, 0x60, 0x93, 0x100, 0x75, 0x31, 0xce,
            0x49, 0x20, 0x56, 0x57, 0xe2, 0xf5, 0x26, 0x2b,
            0x8a, 0xbf, 0xde, 0xd0, 0x83, 0x34, 0xf4, 0x17
        };

        // BWT inverse transform
        void calculate_inverse_bwt(u32* transform, const u8* block, size_t num_bytes) {
            std::array <size_t, 256> count{};
            for (size_t i = 0; i < num_bytes; i++) {
                count[block[i]]++;
            }

            // Calculate cumulative counts
            size_t sum = 0;
            for (size_t i = 0; i < 256; i++) {
                size_t tmp = count[i];
                count[i] = sum;
                sum += tmp;
            }

            // Build transform table
            for (size_t i = 0; i < num_bytes; i++) {
                transform[count[block[i]]++] = static_cast <u32>(i);
            }
        }

        // ============================================================
        // Method 2: LZW compression
        // Based on Unix compress / StuffIt LZW implementation
        // ============================================================

        // LZW constants for StuffIt (14-bit variant)
        constexpr int LZW_MAX_BITS = 14;
        constexpr int LZW_INIT_BITS = 9;
        constexpr int LZW_FIRST = 257; // First free entry
        constexpr int LZW_CLEAR = 256; // Table clear code
        constexpr size_t LZW_HSIZE = 18013; // Hash table size for 14 bits

        class lzw_decoder {
            public:
                lzw_decoder() {
                    // Initialize suffix table with identity for first 256 entries
                    for (int i = 0; i < 256; i++) {
                        tab_suffix_[i] = static_cast <u8>(i);
                        tab_prefix_[i] = 0;
                    }
                }

                result_t <size_t> decompress(byte_span input, mutable_byte_span output) {
                    if (input.empty()) {
                        return 0;
                    }

                    // StuffIt archives store LZW data WITHOUT the 3-byte Unix compress header.
                    // The sit tool uses 14-bit LZW with block compression enabled.
                    // Parameters are fixed: max_bits=14, block_compress=true
                    constexpr int max_bits = LZW_MAX_BITS; // 14
                    constexpr bool block_compress = true;

                    max_maxcode_ = 1 << max_bits;
                    block_compress_ = block_compress;
                    n_bits_ = LZW_INIT_BITS;
                    maxcode_ = (1 << n_bits_) - 1;
                    free_ent_ = block_compress ? LZW_FIRST : 256;
                    clear_flg_ = false;

                    // No header to skip - data starts immediately
                    in_pos_ = 0;
                    in_data_ = input.data();
                    in_size_ = input.size();
                    bit_offset_ = 0;
                    bits_in_buffer_ = 0;

                    byte* out = output.data();
                    byte* out_end = output.data() + output.size();

                    // Get first code
                    int oldcode = get_code(max_bits);
                    if (oldcode < 0) {
                        return 0; // Empty stream
                    }

                    int finchar = oldcode;
                    if (out < out_end) {
                        *out++ = static_cast <byte>(finchar);
                    }

                    int code;
                    while ((code = get_code(max_bits)) >= 0) {
                        // Handle CLEAR code
                        if (code == LZW_CLEAR && block_compress_) {
                            // Reset table
                            for (int i = 0; i < 256; i++) {
                                tab_prefix_[i] = 0;
                            }
                            clear_flg_ = true;
                            free_ent_ = LZW_FIRST;
                            oldcode = -1;
                            continue;
                        }

                        int incode = code;

                        // Special case for KwKwK string
                        if (code >= free_ent_) {
                            if (code > free_ent_ || oldcode < 0) {
                                return std::unexpected(error{error_code::CorruptData, "Bad LZW code"});
                            }
                            stack_[stack_ptr_++] = static_cast <u8>(finchar);
                            code = oldcode;
                        }

                        // Walk the chain to build output
                        while (code >= 256) {
                            if (stack_ptr_ >= sizeof(stack_)) {
                                return std::unexpected(error{error_code::CorruptData, "LZW stack overflow"});
                            }
                            stack_[stack_ptr_++] = tab_suffix_[code];
                            code = tab_prefix_[code];
                        }
                        finchar = tab_suffix_[code];
                        stack_[stack_ptr_++] = static_cast <u8>(finchar);

                        // Output in forward order
                        while (stack_ptr_ > 0 && out < out_end) {
                            *out++ = stack_[--stack_ptr_];
                        }
                        stack_ptr_ = 0;

                        // Add new entry to table
                        if (free_ent_ < max_maxcode_ && oldcode >= 0) {
                            tab_prefix_[free_ent_] = static_cast <u16>(oldcode);
                            tab_suffix_[free_ent_] = static_cast <u8>(finchar);
                            free_ent_++;
                        }

                        oldcode = incode;
                    }

                    return static_cast <size_t>(out - output.data());
                }

            private:
                int get_code(int max_bits) {
                    // Handle code size increase
                    if (clear_flg_ || free_ent_ > maxcode_) {
                        if (free_ent_ > maxcode_) {
                            n_bits_++;
                            if (n_bits_ == max_bits) {
                                maxcode_ = max_maxcode_;
                            } else {
                                maxcode_ = (1 << n_bits_) - 1;
                            }
                        }
                        if (clear_flg_) {
                            n_bits_ = LZW_INIT_BITS;
                            maxcode_ = (1 << n_bits_) - 1;
                            clear_flg_ = false;
                        }
                        // Need to refill buffer when bit size changes
                        bit_offset_ = 0;
                        bits_in_buffer_ = 0;
                    }

                    // Read n_bits_ from stream using LSB-first bit packing
                    // The codes are packed in n_bits-byte groups

                    // If we've exhausted the current group, read a new one
                    if (bit_offset_ >= bits_in_buffer_) {
                        // Read n_bits_ bytes into buffer
                        size_t bytes_to_read = static_cast <size_t>(n_bits_);
                        if (in_pos_ + bytes_to_read > in_size_) {
                            bytes_to_read = in_size_ - in_pos_;
                        }
                        if (bytes_to_read == 0) {
                            return -1;
                        }
                        std::memcpy(gbuf_, in_data_ + in_pos_, bytes_to_read);
                        in_pos_ += bytes_to_read;
                        bit_offset_ = 0;
                        // Round down to integral number of codes
                        bits_in_buffer_ = static_cast <int>((bytes_to_read << 3) - (static_cast <size_t>(n_bits_) - 1));
                    }

                    // Extract code from buffer
                    int r_off = bit_offset_;
                    const u8* bp = gbuf_ + (r_off >> 3);
                    r_off &= 7;

                    // Get first part (low order bits)
                    int gcode = (*bp++ >> r_off);
                    int bits = n_bits_ - (8 - r_off);

                    // Get middle 8-bit parts
                    if (bits >= 8) {
                        gcode |= (*bp++ << (8 - r_off));
                        bits -= 8;
                    }

                    // Get high order bits
                    if (bits > 0) {
                        gcode |= (*bp & ((1 << bits) - 1)) << (n_bits_ - bits);
                    }

                    bit_offset_ += n_bits_;
                    return gcode & ((1 << n_bits_) - 1);
                }

                // LZW tables
                u16 tab_prefix_[1 << LZW_MAX_BITS];
                u8 tab_suffix_[1 << LZW_MAX_BITS];
                u8 stack_[1 << LZW_MAX_BITS];
                size_t stack_ptr_ = 0;

                // Decoding state
                int n_bits_ = LZW_INIT_BITS;
                int maxcode_ = 0;
                int max_maxcode_ = 0;
                int free_ent_ = LZW_FIRST;
                bool block_compress_ = false;
                bool clear_flg_ = false;

                // Input state
                const byte* in_data_ = nullptr;
                size_t in_pos_ = 0;
                size_t in_size_ = 0;
                u8 gbuf_[LZW_MAX_BITS];
                int bit_offset_ = 0;
                int bits_in_buffer_ = 0;
        };

        result_t <size_t> decompress_arsenic(byte_span input, mutable_byte_span output) {
            bit_reader br(input);
            arsenic_decoder decoder;
            decoder.init(br);

            // Initialize models
            arsenic_model initial_model;
            initial_model.init(0, 1, 1, 256);

            arsenic_model selector_model;
            selector_model.init(0, 10, 8, 1024);

            std::array <arsenic_model, 7> mtf_models;
            mtf_models[0].init(2, 3, 8, 1024);
            mtf_models[1].init(4, 7, 4, 1024);
            mtf_models[2].init(8, 15, 4, 1024);
            mtf_models[3].init(16, 31, 4, 1024);
            mtf_models[4].init(32, 63, 2, 1024);
            mtf_models[5].init(64, 127, 2, 1024);
            mtf_models[6].init(128, 255, 1, 1024);

            // Check magic
            if (decoder.next_bit_string(initial_model, 8) != 'A') {
                return std::unexpected(error{error_code::CorruptData, "Invalid Arsenic header"});
            }
            if (decoder.next_bit_string(initial_model, 8) != 's') {
                return std::unexpected(error{error_code::CorruptData, "Invalid Arsenic header"});
            }

            int block_bits = decoder.next_bit_string(initial_model, 4) + 9;
            size_t block_size = static_cast <size_t>(1) << block_bits;

            std::vector <u8> block(block_size);
            std::vector <u32> transform(block_size);
            mtf_decoder mtf;

            byte* out = output.data();
            byte* out_end = output.data() + output.size();

            bool end_of_blocks = decoder.next_symbol(initial_model) != 0;

            int count_state = 0, last = 0;

            while (!end_of_blocks && out < out_end) {
                mtf.reset();

                bool randomized = decoder.next_symbol(initial_model) != 0;
                size_t transform_index = static_cast <size_t>(decoder.next_bit_string(initial_model, block_bits));
                size_t num_bytes = 0;

                // Read block
                while (num_bytes < block_size) {
                    int sel = decoder.next_symbol(selector_model);

                    if (sel == 0 || sel == 1) {
                        // Zero counting
                        size_t zero_state = 1, zero_count = 0;
                        while (sel < 2) {
                            if (sel == 0) zero_count += zero_state;
                            else if (sel == 1) zero_count += 2 * zero_state;
                            zero_state *= 2;
                            sel = decoder.next_symbol(selector_model);
                        }

                        if (num_bytes + zero_count > block_size) break;
                        u8 val = mtf.decode(0);
                        for (size_t i = 0; i < zero_count && num_bytes < block_size; i++) {
                            block[num_bytes++] = val;
                        }
                    }

                    size_t symbol;
                    if (sel == 10) break;
                    else if (sel == 2) symbol = 1;
                    else symbol = static_cast <size_t>(decoder.next_symbol(mtf_models[static_cast <size_t>(sel - 3)]));

                    if (num_bytes >= block_size) break;
                    block[num_bytes++] = mtf.decode(symbol);
                }

                if (transform_index >= num_bytes) {
                    return std::unexpected(error{error_code::CorruptData, "Invalid transform index"});
                }

                // Reset models for next block
                selector_model.reset();
                for (auto& m : mtf_models) m.reset();

                // Check for end of blocks
                if (decoder.next_symbol(initial_model)) {
                    // Read CRC
                    decoder.next_bit_string(initial_model, 32);
                    end_of_blocks = true;
                }

                // Inverse BWT
                calculate_inverse_bwt(transform.data(), block.data(), num_bytes);

                // Output with RLE and optional derandomization
                size_t rand_index = 0;
                int rand_count = RandomizationTable[0];
                int byte_count = 0;

                count_state = 0;

                while (static_cast <size_t>(byte_count) < num_bytes && out < out_end) {
                    transform_index = transform[transform_index];
                    int b = block[transform_index];

                    if (randomized && rand_count == byte_count) {
                        b ^= 1;
                        rand_index = (rand_index + 1) & 255;
                        rand_count += RandomizationTable[rand_index];
                    }

                    byte_count++;

                    if (count_state == 4) {
                        count_state = 0;
                        if (b == 0) continue;
                        // Output 'b' more copies of 'last'
                        for (int repeat = 0; repeat < b && out < out_end; repeat++) {
                            *out++ = static_cast <byte>(last);
                        }
                    } else {
                        if (b == last) count_state++;
                        else {
                            count_state = 1;
                            last = b;
                        }
                        *out++ = static_cast <byte>(b);
                    }
                }
            }

            return static_cast <size_t>(out - output.data());
        }
    } // anonymous namespace

    // Internal fork data
    struct fork_info {
        stuffit::compression_method method = stuffit::compression_method::none;
        bool is_encrypted = false;
        u32 crc_reported = 0;
        u64 uncompressed_size = 0;
        u64 compressed_size = 0;
        u64 data_offset = 0;
    };

    // Internal member data
    struct member_info {
        std::string name;
        std::string full_path;
        bool is_folder = false;
        bool is_end_marker = false;
        u32 filetype = 0;
        u32 creator = 0;
        u16 finder_flags = 0;
        u32 create_time = 0;
        u32 mod_time = 0;
        fork_info data_fork;
        fork_info rsrc_fork;

        // v5 specific
        u64 v5_next_member_pos = 0;
        u64 v5_first_entry_pos = 0;
        u64 v5_num_files = 0;
    };

    struct stuffit_archive::impl {
        byte_span data_;
        stuffit::format_version format_ = stuffit::format_version::old_format;
        std::vector <file_entry> entries_;
        std::vector <member_info> members_;

        // Archive header info
        int num_root_members_ = 0;
        u64 archive_size_ = 0;
        u8 version_ = 0;

        // v5 specific
        u64 v5_first_entry_pos_ = 0;

        result_t <void> parse();
        result_t <void> parse_old_format();
        result_t <void> parse_v5_format();

        result_t <void> parse_old_member(u64 pos, u64& bytes_consumed,
                                         std::vector <std::string>& path_stack, int& subdir_level);
        result_t <void> parse_v5_member(u64 pos, std::vector <std::string>& path_stack,
                                        u64& next_pos);

        result_t <byte_vector> decompress_fork(const fork_info& fork);

        void add_entry(const member_info& member);
    };

    result_t <void> stuffit_archive::impl::parse() {
        if (data_.size() < 22) {
            return std::unexpected(error{error_code::CorruptData, "File too small"});
        }

        // Check signature
        if (std::memcmp(data_.data(), SIG_OLD, 4) == 0) {
            format_ = stuffit::format_version::old_format;
            return parse_old_format();
        } else if (data_.size() >= 8 && std::memcmp(data_.data(), SIG_V5, 8) == 0) {
            format_ = stuffit::format_version::v5_format;
            return parse_v5_format();
        }

        return std::unexpected(error{error_code::CorruptData, "Unknown StuffIt format"});
    }

    result_t <void> stuffit_archive::impl::parse_old_format() {
        // Master header: 22 bytes
        // 0-3: signature "SIT!"
        // 4-5: number of members
        // 6-9: archive size
        // 10-13: expected "rLau"
        // 14: version
        // 15-21: reserved

        num_root_members_ = static_cast <int>(read_u16be(data_.data() + 4));
        archive_size_ = read_u32be(data_.data() + 6);
        version_ = data_[14];

        u64 pos = 22;
        std::vector <std::string> path_stack;
        int subdir_level = 0;

        while (pos + OLD_MEMBER_HEADER_SIZE <= data_.size()) {
            u64 bytes_consumed = 0;
            auto result = parse_old_member(pos, bytes_consumed, path_stack, subdir_level);
            if (!result) {
                // Stop on error, but don't fail if we got some files
                if (entries_.empty()) {
                    return result;
                }
                break;
            }
            if (bytes_consumed == 0) break;
            pos += bytes_consumed;
        }

        return {};
    }

    result_t <void> stuffit_archive::impl::parse_old_member(u64 pos, u64& bytes_consumed,
                                                            std::vector <std::string>& path_stack,
                                                            int& subdir_level) {
        bytes_consumed = 0;

        if (pos + OLD_MEMBER_HEADER_SIZE > data_.size()) {
            return std::unexpected(error{error_code::InputBufferUnderflow, "Truncated member header"});
        }

        const byte* hdr = data_.data() + pos;

        member_info member;

        // Compression methods
        u8 rsrc_cmpr = hdr[0];
        u8 data_cmpr = hdr[1];

        // Decode compression method (old format uses values 0-15 + encrypted flag)
        auto decode_cmpr = [](u8 value, fork_info& fork) {
            if (value >= 32) {
                // Special values: 32=folder, 33=end of folder
                return;
            }
            if (value & 0x10) {
                fork.is_encrypted = true;
                value &= 0x0F;
            }
            fork.method = static_cast <stuffit::compression_method>(value);
        };

        decode_cmpr(rsrc_cmpr, member.rsrc_fork);
        decode_cmpr(data_cmpr, member.data_fork);

        // Check for folder/end marker
        if (rsrc_cmpr == 32 || data_cmpr == 32) {
            member.is_folder = true;
        } else if (rsrc_cmpr == 33 || data_cmpr == 33) {
            member.is_end_marker = true;
            bytes_consumed = OLD_MEMBER_HEADER_SIZE;
            if (subdir_level > 0) {
                subdir_level--;
                if (!path_stack.empty()) path_stack.pop_back();
            }
            return {};
        }

        // Filename (offset 2, length at offset 2, max 63 chars)
        u8 fnlen = hdr[2];
        if (fnlen > 63) fnlen = 63;
        member.name = std::string(reinterpret_cast <const char*>(hdr + 3), fnlen);

        // File type and creator (offset 66)
        member.filetype = read_u32be(hdr + 66);
        member.creator = read_u32be(hdr + 70);

        // Finder flags (offset 74)
        member.finder_flags = read_u16be(hdr + 74);

        // Timestamps (offset 76, 80)
        member.create_time = read_u32be(hdr + 76);
        member.mod_time = read_u32be(hdr + 80);

        // Fork sizes (offset 84)
        member.rsrc_fork.uncompressed_size = read_u32be(hdr + 84);
        member.data_fork.uncompressed_size = read_u32be(hdr + 88);
        member.rsrc_fork.compressed_size = read_u32be(hdr + 92);
        member.data_fork.compressed_size = read_u32be(hdr + 96);

        // CRCs (offset 100)
        member.rsrc_fork.crc_reported = read_u16be(hdr + 100);
        member.data_fork.crc_reported = read_u16be(hdr + 102);

        // Header CRC at offset 110
        // u16 hdr_crc = read_u16be(hdr + 110);

        // Build full path
        path_stack.push_back(member.name);
        member.full_path = "";
        for (size_t i = 0; i < path_stack.size(); i++) {
            if (i > 0) member.full_path += "/";
            member.full_path += path_stack[i];
        }

        bytes_consumed = OLD_MEMBER_HEADER_SIZE;

        // Fork data positions
        u64 data_pos = pos + OLD_MEMBER_HEADER_SIZE;
        member.rsrc_fork.data_offset = data_pos;
        data_pos += member.rsrc_fork.compressed_size;
        member.data_fork.data_offset = data_pos;

        bytes_consumed += member.rsrc_fork.compressed_size + member.data_fork.compressed_size;

        if (member.is_folder) {
            if (subdir_level >= MAX_NESTING_LEVEL) {
                return std::unexpected(error{error_code::CorruptData, "Too many nested folders"});
            }
            subdir_level++;
            // Don't pop path for folders - they stay on stack until end marker
        } else {
            path_stack.pop_back();
        }

        // Store member and create file entry
        members_.push_back(member);
        add_entry(member);

        return {};
    }

    result_t <void> stuffit_archive::impl::parse_v5_format() {
        // v5 archive header starts at offset 0
        // 0-79: text header
        // 80-81: unknown
        // 82: archive version (should be 5)
        // 83: archive flags
        // 84-87: archive size
        // 88-91: unknown
        // 92-93: number of root members
        // 94-97: offset to first root member
        // 98-99: archive CRC

        if (data_.size() < 100) {
            return std::unexpected(error{error_code::CorruptData, "File too small for v5 header"});
        }

        version_ = data_[82];
        u8 flags = data_[83];
        archive_size_ = read_u32be(data_.data() + 84);
        num_root_members_ = static_cast <int>(read_u16be(data_.data() + 92));
        v5_first_entry_pos_ = read_u32be(data_.data() + 94);

        (void)flags; // Unused for now

        std::vector <std::string> path_stack;
        u64 pos = v5_first_entry_pos_;

        for (int i = 0; i < num_root_members_ && pos != 0; i++) {
            u64 next_pos = 0;
            auto result = parse_v5_member(pos, path_stack, next_pos);
            if (!result) {
                if (entries_.empty()) return result;
                break;
            }
            pos = next_pos;
        }

        return {};
    }

    result_t <void> stuffit_archive::impl::parse_v5_member(u64 pos,
                                                           std::vector <std::string>& path_stack,
                                                           u64& next_pos) {
        next_pos = 0;

        if (pos == 0 || pos + 48 > data_.size()) {
            return std::unexpected(error{error_code::InputBufferUnderflow, "Invalid member position"});
        }

        const byte* base = data_.data() + pos;

        // Check magic
        if (read_u32be(base) != 0xa5a5a5a5) {
            return std::unexpected(error{error_code::CorruptData, "Invalid v5 member magic"});
        }

        u8 version = base[4];
        u16 hdr_size = read_u16be(base + 6);
        if (hdr_size < 48 || pos + hdr_size > data_.size()) {
            return std::unexpected(error{error_code::CorruptData, "Invalid header size"});
        }

        member_info member;

        u8 flags = base[9];
        member.is_folder = (flags & 0x40) != 0;
        bool is_encrypted = (flags & 0x20) != 0;

        member.create_time = read_u32be(base + 10);
        member.mod_time = read_u32be(base + 14);

        // prev/next pointers at 18, 22
        next_pos = read_u32be(base + 22);

        u16 fnlen = read_u16be(base + 30);
        if (fnlen > 1024) fnlen = 1024;

        u32 data_length = read_u32be(base + 34);
        u32 data_complen = read_u32be(base + 38);

        // Stream position for reading variable parts
        u64 stream_pos = pos + 46;

        if (member.is_folder) {
            // For directories, bytes 46-47 are number of files
            member.v5_num_files = read_u16be(base + 46);
            member.v5_first_entry_pos = data_length;
            stream_pos = pos + 48;

            // Skip dummy entries
            if (data_length == 0xffffffff) {
                return {};
            }
        } else {
            // Data fork info
            member.data_fork.uncompressed_size = data_length;
            member.data_fork.compressed_size = data_complen;
            member.data_fork.crc_reported = read_u16be(base + 42);
            member.data_fork.method = static_cast <stuffit::compression_method>(base[46]);
            member.data_fork.is_encrypted = is_encrypted;

            u8 passwd_len = base[47];
            stream_pos = pos + 48;

            // Skip password data for encrypted files
            if (is_encrypted && data_length > 0 && passwd_len > 0) {
                stream_pos += passwd_len;
            }
        }

        // Read filename
        if (stream_pos + fnlen <= pos + hdr_size) {
            member.name = std::string(reinterpret_cast <const char*>(data_.data() + stream_pos), fnlen);
            stream_pos += fnlen;
        }

        // Build path
        path_stack.push_back(member.name);
        member.full_path = "";
        for (size_t i = 0; i < path_stack.size(); i++) {
            if (i > 0) member.full_path += "/";
            member.full_path += path_stack[i];
        }

        // For files, calculate data offset accounting for second block and resource fork
        // Second block starts right after the header (at pos + hdr_size)
        u32 rsrc_complen = 0;
        if (!member.is_folder) {
            u64 second_block_pos = pos + hdr_size;

            if (second_block_pos + 22 <= data_.size()) {
                u16 something = read_u16be(data_.data() + second_block_pos);
                bool has_rsrc = (something & 0x01) != 0;

                // Skip: something(2) + unknown(2) + filetype(4) + filecreator(4) + finderflags(2) = 14
                // Plus version-dependent skip: 22 for v1, 18 otherwise
                size_t skip = 14;
                if (version == 1) skip += 22;
                else skip += 18;

                u64 rsrc_info_pos = second_block_pos + skip;

                if (has_rsrc && rsrc_info_pos + 14 <= data_.size()) {
                    // u32 rsrc_length = read_u32be(data_.data() + rsrc_info_pos);
                    rsrc_complen = read_u32be(data_.data() + rsrc_info_pos + 4);
                    // u16 rsrc_crc = read_u16be(data_.data() + rsrc_info_pos + 8);
                    // u8 rsrc_method = data_[rsrc_info_pos + 12];
                    // u8 rsrc_passwd_len = data_[rsrc_info_pos + 13];
                    u8 rsrc_passwd_len = data_[rsrc_info_pos + 13];
                    skip += 14;
                    if (is_encrypted && rsrc_passwd_len > 0) {
                        skip += rsrc_passwd_len;
                    }
                }

                // Data fork starts after second block + resource fork data
                member.data_fork.data_offset = second_block_pos + skip + rsrc_complen;
            } else {
                // Fallback: no second block
                member.data_fork.data_offset = pos + hdr_size;
            }
        }

        members_.push_back(member);
        add_entry(member);

        path_stack.pop_back();

        return {};
    }

    void stuffit_archive::impl::add_entry(const member_info& member) {
        file_entry entry;
        entry.name = member.full_path;
        entry.is_directory = member.is_folder;
        entry.uncompressed_size = member.data_fork.uncompressed_size;
        entry.compressed_size = member.data_fork.compressed_size;
        entry.datetime = mac_time_to_dos(member.mod_time);
        entry.folder_index = static_cast <u32>(members_.size() - 1);
        entries_.push_back(entry);
    }

    result_t <byte_vector> stuffit_archive::impl::decompress_fork(const fork_info& fork) {
        if (fork.compressed_size == 0) {
            return byte_vector{};
        }

        if (fork.data_offset + fork.compressed_size > data_.size()) {
            return std::unexpected(error{error_code::InputBufferUnderflow, "Fork data out of bounds"});
        }

        if (fork.is_encrypted) {
            return std::unexpected(error{error_code::PasswordRequired, "Encrypted files not supported"});
        }

        byte_span compressed(data_.data() + fork.data_offset, fork.compressed_size);
        byte_vector output(fork.uncompressed_size);

        switch (fork.method) {
            case stuffit::compression_method::none:
                if (fork.compressed_size != fork.uncompressed_size) {
                    return std::unexpected(error{error_code::CorruptData, "Size mismatch for uncompressed"});
                }
                std::memcpy(output.data(), compressed.data(), fork.uncompressed_size);
                break;

            case stuffit::compression_method::rle: {
                stuffit_rle_decompressor decompressor;
                auto result = decompressor.decompress_some(compressed, output, true);
                if (!result) return std::unexpected(result.error());
                if (result->bytes_written != fork.uncompressed_size) {
                    output.resize(result->bytes_written);
                }
                break;
            }

            case stuffit::compression_method::huffman: {
                stuffit_huffman_decompressor decompressor;
                auto result = decompressor.decompress_some(compressed, output, true);
                if (!result) return std::unexpected(result.error());
                if (result->bytes_written != fork.uncompressed_size) {
                    output.resize(result->bytes_written);
                }
                break;
            }

            case stuffit::compression_method::lz_huffman: {
                auto result = decompress_method13(compressed, output);
                if (!result) return std::unexpected(result.error());
                if (*result != fork.uncompressed_size) {
                    output.resize(*result);
                }
                break;
            }

            case stuffit::compression_method::arsenic: {
                auto result = decompress_arsenic(compressed, output);
                if (!result) return std::unexpected(result.error());
                if (*result != fork.uncompressed_size) {
                    output.resize(*result);
                }
                break;
            }

            case stuffit::compression_method::lzw: {
                stuffit_lzw_decompressor decompressor;
                auto result = decompressor.decompress_some(compressed, output, true);
                if (!result) return std::unexpected(result.error());
                if (result->bytes_written != fork.uncompressed_size) {
                    output.resize(result->bytes_written);
                }
                break;
            }

            case stuffit::compression_method::deflate: {
                // Method 14 in v5 format uses raw Deflate (RFC 1951)
                inflate_decompressor inflater;
                auto result = inflater.decompress(compressed, output);
                if (!result) return std::unexpected(result.error());
                if (*result != fork.uncompressed_size) {
                    output.resize(*result);
                }
                break;
            }

            case stuffit::compression_method::lzah:
            case stuffit::compression_method::fixed_huffman:
            case stuffit::compression_method::mw:
                return std::unexpected(error{
                    error_code::UnsupportedCompression,
                    "Compression method not yet implemented"
                });

            default:
                return std::unexpected(error{
                    error_code::UnsupportedCompression,
                    "Unknown compression method"
                });
        }

        return output;
    }

    // Public interface

    stuffit_archive::stuffit_archive()
        : pimpl_(std::make_unique <impl>()) {
    }

    stuffit_archive::~stuffit_archive() = default;

    result_t <std::unique_ptr <stuffit_archive>> stuffit_archive::open(byte_span data) {
        auto archive = std::unique_ptr <stuffit_archive>(new stuffit_archive());
        archive->pimpl_->data_ = data;

        auto result = archive->pimpl_->parse();
        if (!result) {
            return std::unexpected(result.error());
        }

        return archive;
    }

    const std::vector <file_entry>& stuffit_archive::files() const {
        return pimpl_->entries_;
    }

    result_t <byte_vector> stuffit_archive::extract(const file_entry& entry) {
        if (entry.folder_index >= pimpl_->members_.size()) {
            return std::unexpected(error{error_code::InvalidParameter, "Invalid entry"});
        }

        const auto& member = pimpl_->members_[entry.folder_index];

        if (member.is_folder) {
            return byte_vector{};
        }

        // Extract data fork (most common use case)
        auto result = pimpl_->decompress_fork(member.data_fork);

        // Report byte-level progress
        if (result.has_value() && byte_progress_cb_) {
            byte_progress_cb_(entry, result->size(), result->size());
        }

        return result;
    }

    stuffit::format_version stuffit_archive::format() const {
        return pimpl_->format_;
    }
} // namespace crate
