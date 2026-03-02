// StuffIt archive format
// Based on deark by Jason Summers (MIT License)
// Compression algorithms based on XADMaster (LGPL v2.1)

#include <crate/formats/stuffit.hh>
#include <crate/core/crc.hh>
#include <crate/compression/inflate.hh>
#include <crate/compression/stuffit_rle.hh>
#include <crate/compression/stuffit_huffman.hh>
#include <crate/compression/stuffit_lzw.hh>
#include <crate/compression/stuffit_method13.hh>
#include <crate/compression/stuffit_arsenic.hh>
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
                                return crate::make_unexpected(error{error_code::CorruptData, "Bad LZW code"});
                            }
                            stack_[stack_ptr_++] = static_cast <u8>(finchar);
                            code = oldcode;
                        }

                        // Walk the chain to build output
                        while (code >= 256) {
                            if (stack_ptr_ >= sizeof(stack_)) {
                                return crate::make_unexpected(error{error_code::CorruptData, "LZW stack overflow"});
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
            return crate::make_unexpected(error{error_code::CorruptData, "File too small"});
        }

        // Check signature
        if (std::memcmp(data_.data(), SIG_OLD, 4) == 0) {
            format_ = stuffit::format_version::old_format;
            return parse_old_format();
        } else if (data_.size() >= 8 && std::memcmp(data_.data(), SIG_V5, 8) == 0) {
            format_ = stuffit::format_version::v5_format;
            return parse_v5_format();
        }

        return crate::make_unexpected(error{error_code::CorruptData, "Unknown StuffIt format"});
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
            return crate::make_unexpected(error{error_code::InputBufferUnderflow, "Truncated member header"});
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
                return crate::make_unexpected(error{error_code::CorruptData, "Too many nested folders"});
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
            return crate::make_unexpected(error{error_code::CorruptData, "File too small for v5 header"});
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
            return crate::make_unexpected(error{error_code::InputBufferUnderflow, "Invalid member position"});
        }

        const byte* base = data_.data() + pos;

        // Check magic
        if (read_u32be(base) != 0xa5a5a5a5) {
            return crate::make_unexpected(error{error_code::CorruptData, "Invalid v5 member magic"});
        }

        u8 version = base[4];
        u16 hdr_size = read_u16be(base + 6);
        if (hdr_size < 48 || pos + hdr_size > data_.size()) {
            return crate::make_unexpected(error{error_code::CorruptData, "Invalid header size"});
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
            return crate::make_unexpected(error{error_code::InputBufferUnderflow, "Fork data out of bounds"});
        }

        if (fork.is_encrypted) {
            return crate::make_unexpected(error{error_code::PasswordRequired, "Encrypted files not supported"});
        }

        byte_span compressed(data_.data() + fork.data_offset, fork.compressed_size);
        byte_vector output(fork.uncompressed_size);

        switch (fork.method) {
            case stuffit::compression_method::none:
                if (fork.compressed_size != fork.uncompressed_size) {
                    return crate::make_unexpected(error{error_code::CorruptData, "Size mismatch for uncompressed"});
                }
                std::memcpy(output.data(), compressed.data(), fork.uncompressed_size);
                break;

            case stuffit::compression_method::rle: {
                stuffit_rle_decompressor decompressor;
                auto result = decompressor.decompress_some(compressed, output, true);
                if (!result) return crate::make_unexpected(result.error());
                if (result->bytes_written != fork.uncompressed_size) {
                    output.resize(result->bytes_written);
                }
                break;
            }

            case stuffit::compression_method::huffman: {
                stuffit_huffman_decompressor decompressor;
                auto result = decompressor.decompress_some(compressed, output, true);
                if (!result) return crate::make_unexpected(result.error());
                if (result->bytes_written != fork.uncompressed_size) {
                    output.resize(result->bytes_written);
                }
                break;
            }

            case stuffit::compression_method::lz_huffman: {
                stuffit_method13_decompressor decompressor;
                auto result = decompressor.decompress_some(compressed, output, true);
                if (!result) return crate::make_unexpected(result.error());
                if (result->bytes_written != fork.uncompressed_size) {
                    output.resize(result->bytes_written);
                }
                break;
            }

            case stuffit::compression_method::arsenic: {
                stuffit_arsenic_decompressor decompressor;
                auto result = decompressor.decompress_some(compressed, output, true);
                if (!result) return crate::make_unexpected(result.error());
                if (result->bytes_written != fork.uncompressed_size) {
                    output.resize(result->bytes_written);
                }
                break;
            }

            case stuffit::compression_method::lzw: {
                stuffit_lzw_decompressor decompressor;
                auto result = decompressor.decompress_some(compressed, output, true);
                if (!result) return crate::make_unexpected(result.error());
                if (result->bytes_written != fork.uncompressed_size) {
                    output.resize(result->bytes_written);
                }
                break;
            }

            case stuffit::compression_method::deflate: {
                // Method 14 in v5 format uses raw Deflate (RFC 1951)
                inflate_decompressor inflater;
                auto result = inflater.decompress(compressed, output);
                if (!result) return crate::make_unexpected(result.error());
                if (*result != fork.uncompressed_size) {
                    output.resize(*result);
                }
                break;
            }

            case stuffit::compression_method::lzah:
            case stuffit::compression_method::fixed_huffman:
            case stuffit::compression_method::mw:
                return crate::make_unexpected(error{
                    error_code::UnsupportedCompression,
                    "Compression method not yet implemented"
                });

            default:
                return crate::make_unexpected(error{
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
            return crate::make_unexpected(result.error());
        }

        return archive;
    }

    const std::vector <file_entry>& stuffit_archive::files() const {
        return pimpl_->entries_;
    }

    result_t <byte_vector> stuffit_archive::extract(const file_entry& entry) {
        if (entry.folder_index >= pimpl_->members_.size()) {
            return crate::make_unexpected(error{error_code::InvalidParameter, "Invalid entry"});
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
