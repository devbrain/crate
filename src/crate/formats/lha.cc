#include <crate/formats/lha.hh>
#include <crate/core/crc.hh>
#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <tuple>
#include <utility>
#include <vector>

namespace crate {

namespace lha {
    // Compression method identifiers
    constexpr char METHOD_LH0[] = "-lh0-";  // Stored (no compression)
    constexpr char METHOD_LH1[] = "-lh1-";  // LZ77 + Huffman (4KB window)
    constexpr char METHOD_LH2[] = "-lh2-";  // LZ77 + dynamic Huffman (8KB)
    constexpr char METHOD_LH3[] = "-lh3-";  // LZ77 + static Huffman (8KB)
    constexpr char METHOD_LH4[] = "-lh4-";  // LZ77 + Huffman (4KB window, newer)
    constexpr char METHOD_LH5[] = "-lh5-";  // LZ77 + Huffman (8KB window)
    constexpr char METHOD_LH6[] = "-lh6-";  // LZ77 + Huffman (32KB window)
    constexpr char METHOD_LH7[] = "-lh7-";  // LZ77 + Huffman (64KB window)
    constexpr char METHOD_LHD[] = "-lhd-";  // Directory
    constexpr char METHOD_LZS[] = "-lzs-";  // LArc (2KB window)
    constexpr char METHOD_LZ4[] = "-lz4-";  // LArc stored
    constexpr char METHOD_LZ5[] = "-lz5-";  // LArc (4KB window)
    constexpr char METHOD_PM0[] = "-pm0-";  // PMarc stored
    constexpr char METHOD_PM2[] = "-pm2-";  // PMarc

    struct CRATE_EXPORT file_header {
        u8 header_level = 0;
        char method[6] = {0};
        u64 compressed_size = 0;
        u64 original_size = 0;
        u32 timestamp = 0;
        u16 crc = 0;
        u8 os_type = 0;
        std::string path;
        std::string filename;

        [[nodiscard]] std::string full_path() const {
            if (path.empty()) return filename;
            return path + filename;
        }

        [[nodiscard]] bool is_directory() const {
            return std::memcmp(method, METHOD_LHD, 5) == 0;
        }
    };

    // CRC-16 for LHA (CCITT polynomial)
    class CRATE_EXPORT crc_16 {
    public:
        static u16 calculate(byte_span data) {
            u16 crc = 0;
            for (u8 value : data) {
                crc = update(crc, value);
            }
            return crc;
        }

        static u16 update(u16 crc, u8 value) {
            crc ^= static_cast<u16>(value);
            for (int i = 0; i < 8; i++) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ 0xA001;
                } else {
                    crc >>= 1;
                }
            }
            return crc;
        }
    };

    // Bit stream reader for decompression
    class CRATE_EXPORT bit_reader {
    public:
        explicit bit_reader(byte_span data) : data_(data) {}

        int read_bits(unsigned int n) {
            if (n == 0) return 0;

            while (bits_ < n) {
                if (pos_ >= data_.size()) return -1;
                bit_buffer_ |= static_cast<u32>(data_[pos_++]) << (24 - bits_);
                bits_ += 8;
            }

            int result = static_cast<int>(bit_buffer_ >> (32 - n));
            bit_buffer_ <<= n;
            bits_ -= n;
            return result;
        }

        int read_bit() { return read_bits(1); }

        // Peek at n bits without consuming them
        int peek_bits(unsigned int n) {
            if (n == 0) return 0;

            while (bits_ < n) {
                if (pos_ >= data_.size()) return -1;
                bit_buffer_ |= static_cast<u32>(data_[pos_++]) << (24 - bits_);
                bits_ += 8;
            }

            return static_cast<int>(bit_buffer_ >> (32 - n));
        }

        [[nodiscard]] size_t position() const { return pos_; }
        [[nodiscard]] bool eof() const { return pos_ >= data_.size() && bits_ == 0; }

    private:
        byte_span data_;
        size_t pos_ = 0;
        u32 bit_buffer_ = 0;
        unsigned int bits_ = 0;
    };

    // Huffman tree for LHA decompression
    template<size_t MaxCodes>
    class CRATE_EXPORT huffman_tree {
    public:
        static constexpr u16 LEAF = 0x8000;

        void init() {
            std::fill(tree_.begin(), tree_.end(), LEAF);
        }

        void set_single(u16 code) {
            tree_[0] = code | LEAF;
        }

        bool build(const u8* lengths, size_t num_codes) {
            if (num_codes > MaxCodes) return false;

            init();

            // Find max length
            u8 max_len = 0;
            for (size_t i = 0; i < num_codes; i++) {
                if (lengths[i] > max_len) max_len = lengths[i];
            }

            if (max_len == 0) return true;

            // Build tree level by level
            size_t next_entry = 0;
            size_t allocated = 1;

            for (u8 len = 1; len <= max_len; len++) {
                // Expand queue - allocate children for pending nodes
                size_t end_offset = allocated;
                while (next_entry < end_offset) {
                    if (allocated + 2 > tree_.size()) return false;
                    tree_[next_entry] = static_cast<u16>(allocated);
                    allocated += 2;
                    next_entry++;
                }

                // Add codes with this length
                for (size_t i = 0; i < num_codes; i++) {
                    if (lengths[i] == len) {
                        if (next_entry >= allocated) return false;
                        tree_[next_entry++] = static_cast<u16>(i) | LEAF;
                    }
                }
            }

            return true;
        }

        int read(bit_reader& reader) const {
            u16 code = tree_[0];

            while ((code & LEAF) == 0) {
                int bit = reader.read_bit();
                if (bit < 0) return -1;
                code = tree_[static_cast <size_t>(code) + static_cast <size_t>(bit)];
            }

            return code & ~LEAF;
        }

    private:
        std::array<u16, MaxCodes * 2> tree_;
    };

    // LH5/LH6/LH7 decoder (new-style LHA compression)
    template<int HistoryBits, int OffsetBits>
    class CRATE_EXPORT lh_new_decoder {
    public:
        static constexpr size_t RING_SIZE = 1 << HistoryBits;
        static constexpr size_t NUM_CODES = 510;
        static constexpr size_t MAX_TEMP_CODES = 31;
        static constexpr size_t MAX_OFFSET_CODES = (1 << OffsetBits) - 1;
        static constexpr size_t COPY_THRESHOLD = 3;

        lh_new_decoder() { reset(); }

        void reset() {
            std::fill(ringbuf_.begin(), ringbuf_.end(), ' ');
            ringbuf_pos_ = 0;
            block_remaining_ = 0;
        }

        result_t<byte_vector> decompress(byte_span input, size_t output_size) {
            bit_reader reader(input);
            byte_vector output;
            output.reserve(output_size);

            while (output.size() < output_size) {
                // Start new block if needed
                while (block_remaining_ == 0) {
                    if (!start_new_block(reader)) {
                        if (output.size() >= output_size) break;
                        return std::unexpected(error{error_code::CorruptData, "Failed to read LHA block"});
                    }
                }

                if (block_remaining_ == 0) break;
                block_remaining_--;

                // Read code
                int code = code_tree_.read(reader);
                if (code < 0) {
                    return std::unexpected(error{error_code::CorruptData, "Failed to read LHA code"});
                }

                if (code < 256) {
                    // Literal byte
                    output_byte(output, static_cast<u8>(code));
                } else {
                    // Copy from history
                    size_t copy_count_size = static_cast <size_t>(
                        static_cast <unsigned>(code - 256)) + COPY_THRESHOLD;

                    int offset = read_offset(reader);
                    if (offset < 0) {
                        return std::unexpected(error{error_code::CorruptData, "Failed to read LHA offset"});
                    }

                    // Validate offset is within ring buffer bounds
                    if (static_cast<size_t>(offset) >= RING_SIZE) {
                        return std::unexpected(error{error_code::CorruptData, "LHA offset exceeds ring buffer size"});
                    }

                    size_t start = (ringbuf_pos_ + RING_SIZE - static_cast <size_t>(offset) - 1) % RING_SIZE;
                    for (size_t i = 0; i < copy_count_size && output.size() < output_size; i++) {
                        output_byte(output, ringbuf_[(start + i) % RING_SIZE]);
                    }
                }
            }

            return output;
        }

    private:
        void output_byte(byte_vector& output, u8 value) {
            output.push_back(value);
            ringbuf_[ringbuf_pos_] = value;
            ringbuf_pos_ = (ringbuf_pos_ + 1) % RING_SIZE;
        }

        bool start_new_block(bit_reader& reader) {
            int len = reader.read_bits(16);
            if (len < 0) return false;

            block_remaining_ = static_cast<size_t>(len);

            // Read temp table
            if (!read_temp_table(reader)) return false;

            // Read code table
            if (!read_code_table(reader)) return false;

            // Read offset table
            if (!read_offset_table(reader)) return false;

            return true;
        }

        int read_length_value(bit_reader& reader) {
            int len = reader.read_bits(3);
            if (len < 0) return -1;

            if (len == 7) {
                for (;;) {
                    int bit = reader.read_bit();
                    if (bit < 0) return -1;
                    if (bit == 0) break;
                    len++;
                }
            }
            return len;
        }

        bool read_temp_table(bit_reader& reader) {
            int n = reader.read_bits(5);
            if (n < 0) return false;

            if (n == 0) {
                int code = reader.read_bits(5);
                if (code < 0) return false;
                temp_tree_.set_single(static_cast<u16>(code));
                return true;
            }

            if (static_cast<size_t>(n) > MAX_TEMP_CODES) n = MAX_TEMP_CODES;

            u8 lengths[MAX_TEMP_CODES] = {0};
            for (int i = 0; i < n; i++) {
                int len = read_length_value(reader);
                if (len < 0) return false;
                lengths[i] = static_cast<u8>(len);

                // Skip field after first 3
                if (i == 2) {
                    int skip = reader.read_bits(2);
                    if (skip < 0) return false;
                    for (int j = 0; j < skip && i + 1 < n; j++) {
                        i++;
                        lengths[i] = 0;
                    }
                }
            }

            return temp_tree_.build(lengths, static_cast <size_t>(n));
        }

        bool read_code_table(bit_reader& reader) {
            int n = reader.read_bits(9);
            if (n < 0) return false;

            if (n == 0) {
                int code = reader.read_bits(9);
                if (code < 0) return false;
                code_tree_.set_single(static_cast<u16>(code));
                return true;
            }

            if (static_cast<size_t>(n) > NUM_CODES) n = NUM_CODES;

            u8 lengths[NUM_CODES] = {0};
            int i = 0;

            while (i < n) {
                int code = temp_tree_.read(reader);
                if (code < 0) return false;

                if (code <= 2) {
                    // Skip codes
                    int skip_count;
                    if (code == 0) {
                        skip_count = 1;
                    } else if (code == 1) {
                        int extra = reader.read_bits(4);
                        if (extra < 0) return false;
                        skip_count = extra + 3;
                    } else {
                        int extra = reader.read_bits(9);
                        if (extra < 0) return false;
                        skip_count = extra + 20;
                    }

                    for (int j = 0; j < skip_count && i < n; j++) {
                        lengths[i++] = 0;
                    }
                } else {
                    lengths[i++] = static_cast<u8>(code - 2);
                }
            }

            return code_tree_.build(lengths, static_cast <size_t>(n));
        }

        bool read_offset_table(bit_reader& reader) {
            int n = reader.read_bits(OffsetBits);
            if (n < 0) return false;

            if (n == 0) {
                int code = reader.read_bits(OffsetBits);
                if (code < 0) return false;
                offset_tree_.set_single(static_cast<u16>(code));
                return true;
            }

            if (static_cast<size_t>(n) > MAX_OFFSET_CODES) n = MAX_OFFSET_CODES;

            u8 lengths[MAX_OFFSET_CODES] = {0};
            for (int i = 0; i < n; i++) {
                int len = read_length_value(reader);
                if (len < 0) return false;
                lengths[i] = static_cast<u8>(len);
            }

            return offset_tree_.build(lengths, static_cast <size_t>(n));
        }

        int read_offset(bit_reader& reader) {
            int bits = offset_tree_.read(reader);
            if (bits < 0) return -1;

            if (bits == 0) return 0;
            if (bits == 1) return 1;

            int extra = reader.read_bits(static_cast <unsigned>(bits - 1));
            if (extra < 0) return -1;

            return extra + (1 << (bits - 1));
        }

        std::array<u8, RING_SIZE> ringbuf_{};
        size_t ringbuf_pos_ = 0;
        size_t block_remaining_ = 0;

        huffman_tree<MAX_TEMP_CODES> temp_tree_;
        huffman_tree<NUM_CODES> code_tree_;
        huffman_tree<MAX_OFFSET_CODES> offset_tree_;
    };

    // LZS decoder (LArc, 2KB window)
    class CRATE_EXPORT lzs_decoder {
    public:
        static constexpr size_t RING_SIZE = 2048;
        static constexpr size_t OFFSET_MASK = RING_SIZE - 1;
        static constexpr size_t START_OFFSET = 17;
        static constexpr size_t THRESHOLD = 2;

        static result_t<byte_vector> decompress(byte_span input, size_t output_size) {
            bit_reader reader(input);
            byte_vector output;
            output.reserve(output_size);

            std::array<u8, RING_SIZE> ringbuf{};
            std::fill(ringbuf.begin(), ringbuf.end(), ' ');
            size_t ringbuf_pos = RING_SIZE - START_OFFSET;

            while (output.size() < output_size) {
                int bit = reader.read_bit();
                if (bit < 0) break;

                if (bit == 1) {
                    // Literal byte
                    int value = reader.read_bits(8);
                    if (value < 0) break;

                    output.push_back(static_cast<u8>(value));
                    ringbuf[ringbuf_pos] = static_cast<u8>(value);
                    ringbuf_pos = (ringbuf_pos + 1) & OFFSET_MASK;
                } else {
                    // Copy from history - pos is absolute position
                    int pos = reader.read_bits(11);
                    if (pos < 0) break;

                    int length = reader.read_bits(4);
                    if (length < 0) break;
                    size_t length_value = static_cast <size_t>(length) + THRESHOLD;

                    size_t pos_value = static_cast <size_t>(pos);
                    for (size_t i = 0; i < length_value && output.size() < output_size; i++) {
                        u8 value = ringbuf[(pos_value + i) & OFFSET_MASK];
                        output.push_back(value);
                        ringbuf[ringbuf_pos] = value;
                        ringbuf_pos = (ringbuf_pos + 1) & OFFSET_MASK;
                    }
                }
            }

            return output;
        }
    };

    // LZ5 decoder (LArc, 4KB window)
    class CRATE_EXPORT lz5_decoder {
    public:
        static constexpr size_t RING_SIZE = 4096;
        static constexpr size_t OFFSET_MASK = RING_SIZE - 1;
        static constexpr size_t START_OFFSET = 18;
        static constexpr size_t THRESHOLD = 3;

        static result_t<byte_vector> decompress(byte_span input, size_t output_size) {
            byte_vector output;
            output.reserve(output_size);

            std::array<u8, RING_SIZE> ringbuf{};
            init_ringbuf(ringbuf);
            size_t ringbuf_pos = RING_SIZE - START_OFFSET;

            size_t pos = 0;

            while (output.size() < output_size && pos < input.size()) {
                u8 flags = input[pos++];

                for (int bit = 0; bit < 8 && output.size() < output_size; bit++) {
                    if (flags & (1 << bit)) {
                        // Literal byte
                        if (pos >= input.size()) break;
                        u8 value = input[pos++];

                        output.push_back(value);
                        ringbuf[ringbuf_pos] = value;
                        ringbuf_pos = (ringbuf_pos + 1) & OFFSET_MASK;
                    } else {
                        // Copy from history
                        if (pos + 2 > input.size()) break;

                        u8 b1 = input[pos++];
                        u8 b2 = input[pos++];

                        // seqstart = ((cmd[1] & 0xf0) << 4) | cmd[0]
                        size_t seqstart = ((static_cast<size_t>(b2) & 0xF0) << 4) | b1;
                        size_t seqlen = (b2 & 0x0F) + THRESHOLD;

                        for (size_t i = 0; i < seqlen && output.size() < output_size; i++) {
                            u8 value = ringbuf[(seqstart + i) & OFFSET_MASK];
                            output.push_back(value);
                            ringbuf[ringbuf_pos] = value;
                            ringbuf_pos = (ringbuf_pos + 1) & OFFSET_MASK;
                        }
                    }
                }
            }

            return output;
        }

    private:
        static void init_ringbuf(std::array<u8, RING_SIZE>& ringbuf) {
            u8* p = ringbuf.data();

            // For each byte value, include a run of 13 bytes with that value
            for (int i = 0; i < 256; i++) {
                for (int j = 0; j < 13; j++) {
                    *p++ = static_cast<u8>(i);
                }
            }

            // All byte values ascending
            for (int i = 0; i < 256; i++) {
                *p++ = static_cast<u8>(i);
            }

            // All byte values descending
            for (int i = 0; i < 256; i++) {
                *p++ = static_cast<u8>(255 - i);
            }

            // 128 zeros
            for (int i = 0; i < 128; i++) {
                *p++ = 0;
            }

            // 110 spaces
            for (int i = 0; i < 110; i++) {
                *p++ = ' ';
            }

            // Final 18 zeros (START_OFFSET)
            for (int i = 0; i < 18; i++) {
                *p++ = 0;
            }
        }
    };

    // LH1 decoder (old-style LHA with adaptive Huffman)
    // Uses 4KB sliding window and dynamic Huffman tree
    // Based on lhasa's lh1_decoder.c implementation
    class CRATE_EXPORT lh1_decoder {
    public:
        static constexpr size_t RING_SIZE = 4096;        // 4KB window
        static constexpr size_t NUM_CODES = 314;         // 256 literals + 58 copy lengths
        static constexpr size_t NUM_TREE_NODES = NUM_CODES * 2 - 1;
        static constexpr size_t NUM_OFFSETS = 64;        // Offset table size
        static constexpr size_t COPY_THRESHOLD = 3;      // Minimum copy length
        static constexpr u16 TREE_REORDER_LIMIT = 32 * 1024;  // Tree rebuild threshold
        static constexpr unsigned int MIN_OFFSET_LENGTH = 3;

        result_t<byte_vector> decompress(byte_span input, size_t output_size) {
            bit_reader reader(input);
            byte_vector output;
            output.reserve(output_size);

            // Initialize
            init_groups();
            init_tree();
            init_offset_table();
            ringbuf_.fill(' ');
            ringbuf_pos_ = 0;

            while (output.size() < output_size) {
                int code = read_code(reader);
                if (code < 0) {
                    if (output.size() >= output_size) break;
                    return std::unexpected(error{error_code::CorruptData, "Failed to read LH1 code"});
                }

                if (code < 256) {
                    // Literal byte
                    output_byte(output, static_cast<u8>(code));
                } else {
                    // Copy from history
                    int offset = read_offset(reader);
                    if (offset < 0) {
                        return std::unexpected(error{error_code::CorruptData, "Failed to read LH1 offset"});
                    }

                    unsigned count = static_cast <unsigned>(code - 256);
                    size_t count_size = static_cast <size_t>(count) + COPY_THRESHOLD;
                    size_t start = (ringbuf_pos_ + RING_SIZE - static_cast<size_t>(offset) - 1) % RING_SIZE;

                    for (size_t i = 0; i < count_size && output.size() < output_size; i++) {
                        output_byte(output, ringbuf_[(start + i) % RING_SIZE]);
                    }
                }
            }

            return output;
        }

    private:
        struct CRATE_EXPORT Node {
            unsigned int is_leaf : 1 = 0;
            unsigned int child_index : 15 = 0;  // Code value for leaves, right child for internal
            u16 parent = 0;
            u16 freq = 0;
            u16 group = 0;
        };

        std::array<Node, NUM_TREE_NODES> nodes_;
        std::array<u16, NUM_CODES> leaf_nodes_;
        std::array<u16, NUM_TREE_NODES> groups_;  // Free group pool
        std::array<u16, NUM_TREE_NODES> group_leader_;
        unsigned int num_groups_ = 0;
        std::array<u8, 256> offset_lookup_;
        std::array<u8, NUM_OFFSETS> offset_lengths_;
        std::array<u8, RING_SIZE> ringbuf_;
        size_t ringbuf_pos_ = 0;

        void output_byte(byte_vector& output, u8 value) {
            output.push_back(value);
            ringbuf_[ringbuf_pos_] = value;
            ringbuf_pos_ = (ringbuf_pos_ + 1) % RING_SIZE;
        }

        u16 alloc_group() {
            return groups_[num_groups_++];
        }

        void free_group(u16 group) {
            groups_[--num_groups_] = group;
        }

        void init_groups() {
            for (size_t i = 0; i < NUM_TREE_NODES; i++) {
                groups_[i] = static_cast<u16>(i);
            }
            num_groups_ = 0;
        }

        void init_tree() {
            // Leaf nodes at the end of the table
            int node_index = static_cast <int>(NUM_TREE_NODES) - 1;
            u16 leaf_group = alloc_group();

            for (size_t i = 0; i < NUM_CODES; i++) {
                Node& node = nodes_[static_cast <size_t>(node_index)];
                node.is_leaf = 1;
                node.child_index = static_cast <u16>(i & 0x7FFFu);
                node.freq = 1;
                node.group = leaf_group;
                group_leader_[leaf_group] = static_cast <u16>(node_index);
                leaf_nodes_[i] = static_cast <u16>(node_index);
                node_index--;
            }

            // Build intermediate nodes
            unsigned int child = NUM_TREE_NODES - 1;

            while (node_index >= 0) {
                Node& node = nodes_[static_cast <size_t>(node_index)];
                node.is_leaf = 0;
                node.child_index = static_cast <u16>(child & 0x7FFFu);
                nodes_[child].parent = static_cast <u16>(node_index);
                nodes_[child - 1].parent = static_cast <u16>(node_index);
                node.freq = static_cast <u16>(nodes_[child].freq + nodes_[child - 1].freq);

                // Same freq as next node? Same group. Otherwise allocate new.
                if (node_index < static_cast <int>(NUM_TREE_NODES) - 1 &&
                    node.freq == nodes_[static_cast <size_t>(node_index + 1)].freq) {
                    node.group = nodes_[static_cast <size_t>(node_index + 1)].group;
                } else {
                    node.group = alloc_group();
                }
                group_leader_[node.group] = static_cast <u16>(node_index);

                node_index--;
                child -= 2;
            }
        }

        void init_offset_table() {
            // Offset frequency distribution: number of codes at each bit length
            static constexpr unsigned int offset_fdist[] = {
                1,   // 3 bits
                3,   // 4 bits
                8,   // 5 bits
                12,  // 6 bits
                24,  // 7 bits
                16,  // 8 bits
            };

            u8 code = 0;
            u8 offset = 0;

            for (size_t i = 0; i < sizeof(offset_fdist) / sizeof(offset_fdist[0]); i++) {
                unsigned int len = static_cast <unsigned int>(i) + MIN_OFFSET_LENGTH;
                u8 iterbit = static_cast<u8>(1 << (8 - len));

                for (unsigned int j = 0; j < offset_fdist[i]; j++) {
                    // Fill lookup table entries for this offset
                    u8 mask = static_cast<u8>(iterbit - 1);
                    for (unsigned int k = 0; (k & ~mask) == 0; k++) {
                        offset_lookup_[code | k] = offset;
                    }
                    offset_lengths_[offset] = static_cast<u8>(len);

                    code = static_cast<u8>(code + iterbit);
                    offset++;
                }
            }
        }

        int read_code(bit_reader& reader) {
            size_t node_index = 0;

            while (node_index < nodes_.size() && !nodes_[node_index].is_leaf) {
                int bit = reader.read_bit();
                if (bit < 0) return -1;
                u16 child_index = nodes_[node_index].child_index;
                if (child_index >= NUM_TREE_NODES) return -1;
                node_index = static_cast<size_t>(child_index) - static_cast<size_t>(bit);
            }
            if (node_index >= nodes_.size()) return -1;

            u16 code = static_cast<u16>(nodes_[node_index].child_index);
            increment_for_code(code);
            return static_cast<int>(code);
        }

        u16 make_group_leader(u16 node_index) {
            u16 group = nodes_[node_index].group;
            u16 leader_index = group_leader_[group];

            if (leader_index == node_index) return node_index;

            Node& node = nodes_[node_index];
            Node& leader = nodes_[leader_index];

            // Swap leaf and child_index (can't use std::swap with bit-fields)
            unsigned int tmp_leaf = node.is_leaf;
            node.is_leaf = leader.is_leaf;
            leader.is_leaf = tmp_leaf ? 1u : 0u;

            unsigned int tmp_child = node.child_index;
            node.child_index = leader.child_index;
            leader.child_index = tmp_child & 0x7FFFu;

            // Update back-references
            if (node.is_leaf) {
                if (node.child_index < NUM_CODES) {
                    leaf_nodes_[node.child_index] = node_index;
                } else {
                    return node_index;
                }
            } else {
                if (node.child_index >= NUM_TREE_NODES || node.child_index == 0) {
                    return node_index;
                }
                nodes_[node.child_index].parent = node_index;
                nodes_[node.child_index - 1].parent = node_index;
            }

            if (leader.is_leaf) {
                if (leader.child_index < NUM_CODES) {
                    leaf_nodes_[leader.child_index] = leader_index;
                } else {
                    return node_index;
                }
            } else {
                if (leader.child_index >= NUM_TREE_NODES || leader.child_index == 0) {
                    return node_index;
                }
                nodes_[leader.child_index].parent = leader_index;
                nodes_[leader.child_index - 1].parent = leader_index;
            }

            return leader_index;
        }

        void increment_node_freq(u16 node_index) {
            Node& node = nodes_[node_index];
            Node& other = nodes_[node_index - 1];

            node.freq++;

            // If node is part of a group with other nodes
            if (node_index < NUM_TREE_NODES - 1 &&
                node.group == nodes_[node_index + 1].group) {
                // Next node becomes the leader
                group_leader_[node.group]++;

                // Join left group or start new group
                if (node.freq == other.freq) {
                    node.group = other.group;
                } else {
                    node.group = alloc_group();
                    group_leader_[node.group] = node_index;
                }
            } else {
                // Single-node group - might need to join left group
                if (node.freq == other.freq) {
                    free_group(node.group);
                    node.group = other.group;
                }
            }
        }

        void reconstruct_tree() {
            // Gather leaf nodes at start, halving frequencies
            Node* leaf = &nodes_[0];
            for (size_t i = 0; i < NUM_TREE_NODES; i++) {
                if (nodes_[i].is_leaf) {
                    leaf->is_leaf = 1;
                    leaf->child_index = nodes_[i].child_index;
                    leaf->freq = static_cast<u16>((nodes_[i].freq + 1) / 2);
                    leaf++;
                }
            }

            // Rebuild tree
            Node* leaf_ptr = &nodes_[NUM_CODES - 1];
            unsigned int child = NUM_TREE_NODES - 1;
            int i = NUM_TREE_NODES - 1;

            while (i >= 0) {
                // Need at least 2 children
                while (static_cast<int>(child) - i < 2) {
                    size_t i_index = static_cast <size_t>(i);
                    nodes_[i_index] = *leaf_ptr;
                    if (leaf_ptr->child_index < NUM_CODES) {
                        leaf_nodes_[leaf_ptr->child_index] = static_cast <u16>(i);
                    }
                    i--;
                    leaf_ptr--;
                }

                u16 freq = static_cast<u16>(nodes_[child].freq + nodes_[child - 1].freq);

                // Copy more leaves while their freq >= branch freq
                while (leaf_ptr >= &nodes_[0] && freq >= leaf_ptr->freq) {
                    size_t i_index = static_cast <size_t>(i);
                    nodes_[i_index] = *leaf_ptr;
                    if (leaf_ptr->child_index < NUM_CODES) {
                        leaf_nodes_[leaf_ptr->child_index] = static_cast <u16>(i);
                    }
                    i--;
                    leaf_ptr--;
                }

                // Insert branch node
                size_t i_index = static_cast <size_t>(i);
                nodes_[i_index].is_leaf = 0;
                nodes_[i_index].freq = freq;
                nodes_[i_index].child_index = static_cast <u16>(child & 0x7FFFu);
                nodes_[child].parent = static_cast <u16>(i);
                nodes_[child - 1].parent = static_cast <u16>(i);
                i--;
                child -= 2;
            }

            // Rebuild groups
            init_groups();
            u16 group = alloc_group();
            nodes_[0].group = group;
            group_leader_[group] = 0;

            for (size_t j = 1; j < NUM_TREE_NODES; j++) {
                if (nodes_[j].freq == nodes_[j - 1].freq) {
                    nodes_[j].group = nodes_[j - 1].group;
                } else {
                    group = alloc_group();
                    nodes_[j].group = group;
                    group_leader_[group] = static_cast<u16>(j);
                }
            }
        }

        void increment_for_code(u16 code) {
            // Check for tree reorder
            if (nodes_[0].freq >= TREE_REORDER_LIMIT) {
                reconstruct_tree();
            }

            nodes_[0].freq++;

            if (code >= NUM_CODES) return;
            u16 node_index = leaf_nodes_[code];
            while (node_index != 0) {
                node_index = make_group_leader(node_index);
                increment_node_freq(node_index);
                node_index = nodes_[node_index].parent;
            }
        }

        int read_offset(bit_reader& reader) {
            int peek = reader.peek_bits(8);
            if (peek < 0) return -1;

            if (peek >= static_cast<int>(offset_lookup_.size())) return -1;
            u8 offset = offset_lookup_[static_cast <size_t>(peek)];
            reader.read_bits(offset_lengths_[offset]);

            int offset2 = reader.read_bits(6);
            if (offset2 < 0) return -1;

            return (static_cast<int>(offset) << 6) | offset2;
        }
    };

    // Decoder type aliases
    using lh5_decoder = lh_new_decoder<14, 4>;  // 16KB window
    using lh6_decoder = lh_new_decoder<16, 5>;  // 64KB window
    using lh7_decoder = lh_new_decoder<17, 5>;  // 128KB window

    // LK7 decoder (LHark's -lh7- variant)
    // Uses different offset encoding and copy count decoding
    class CRATE_EXPORT lk7_decoder {
    public:
        static constexpr size_t RING_SIZE = 1 << 16;  // 64KB
        static constexpr size_t NUM_CODES = 289;
        static constexpr size_t MAX_TEMP_CODES = 31;
        static constexpr size_t MAX_OFFSET_CODES = 63;  // 6 bits
        static constexpr size_t COPY_THRESHOLD = 3;

        lk7_decoder() { reset(); }

        void reset() {
            std::fill(ringbuf_.begin(), ringbuf_.end(), ' ');
            ringbuf_pos_ = 0;
            block_remaining_ = 0;
        }

        result_t<byte_vector> decompress(byte_span input, size_t output_size) {
            bit_reader reader(input);
            byte_vector output;
            output.reserve(output_size);

            while (output.size() < output_size) {
                // Start new block if needed
                while (block_remaining_ == 0) {
                    if (!start_new_block(reader)) {
                        if (output.size() >= output_size) break;
                        return std::unexpected(error{error_code::CorruptData, "Failed to read LK7 block"});
                    }
                }

                if (block_remaining_ == 0) break;
                block_remaining_--;

                // Read code
                int code = code_tree_.read(reader);
                if (code < 0) {
                    return std::unexpected(error{error_code::CorruptData, "Failed to read LK7 code"});
                }

                if (code < 256) {
                    // Literal byte
                    output_byte(output, static_cast<u8>(code));
                } else {
                    // Copy from history - LHark uses different copy count encoding
                    int copy_count = decode_copy_count(reader, code);
                    if (copy_count < 0) {
                        return std::unexpected(error{error_code::CorruptData, "Failed to decode LK7 copy count"});
                    }

                    int offset = read_offset(reader);
                    if (offset < 0) {
                        return std::unexpected(error{error_code::CorruptData, "Failed to read LK7 offset"});
                    }

                    // Validate offset is within ring buffer bounds
                    if (static_cast<size_t>(offset) >= RING_SIZE) {
                        return std::unexpected(error{error_code::CorruptData, "LK7 offset exceeds ring buffer size"});
                    }

                    size_t start = (ringbuf_pos_ + RING_SIZE - static_cast <size_t>(offset) - 1) % RING_SIZE;
                    for (size_t i = 0; i < static_cast <size_t>(copy_count) &&
                                       output.size() < output_size; i++) {
                        output_byte(output, ringbuf_[(start + i) % RING_SIZE]);
                    }
                }
            }

            return output;
        }

    private:
        void output_byte(byte_vector& output, u8 value) {
            output.push_back(value);
            ringbuf_[ringbuf_pos_] = value;
            ringbuf_pos_ = (ringbuf_pos_ + 1) % RING_SIZE;
        }

        // LHark-specific copy count decoding
        static int decode_copy_count(bit_reader& reader, int code) {
            if (code < 264) {
                return code - 256 + static_cast <int>(COPY_THRESHOLD);
            } else if (code < 288) {
                int num_low_bits = (code - 260) / 4;
                int low_bits = reader.read_bits(static_cast <unsigned>(num_low_bits));
                if (low_bits < 0) return -1;
                return ((4 + (code % 4)) << num_low_bits) + low_bits + 3;
            } else {
                return 514;
            }
        }

        bool start_new_block(bit_reader& reader) {
            int len = reader.read_bits(16);
            if (len < 0) return false;

            block_remaining_ = static_cast<size_t>(len);

            if (!read_temp_table(reader)) return false;
            if (!read_code_table(reader)) return false;
            if (!read_offset_table(reader)) return false;

            return true;
        }

        static int read_length_value(bit_reader& reader) {
            int len = reader.read_bits(3);
            if (len < 0) return -1;

            if (len == 7) {
                for (;;) {
                    int bit = reader.read_bit();
                    if (bit < 0) return -1;
                    if (bit == 0) break;
                    len++;
                }
            }
            return len;
        }

        bool read_temp_table(bit_reader& reader) {
            int n = reader.read_bits(5);
            if (n < 0) return false;

            if (n == 0) {
                int code = reader.read_bits(5);
                if (code < 0) return false;
                temp_tree_.set_single(static_cast<u16>(code));
                return true;
            }

            if (static_cast<size_t>(n) > MAX_TEMP_CODES) n = MAX_TEMP_CODES;

            u8 lengths[MAX_TEMP_CODES] = {0};
            for (int i = 0; i < n; i++) {
                int len = read_length_value(reader);
                if (len < 0) return false;
                lengths[i] = static_cast<u8>(len);

                if (i == 2) {
                    int skip = reader.read_bits(2);
                    if (skip < 0) return false;
                    for (int j = 0; j < skip && i + 1 < n; j++) {
                        i++;
                        lengths[i] = 0;
                    }
                }
            }

            return temp_tree_.build(lengths, static_cast <size_t>(n));
        }

        bool read_code_table(bit_reader& reader) {
            int n = reader.read_bits(9);
            if (n < 0) return false;

            if (n == 0) {
                int code = reader.read_bits(9);
                if (code < 0) return false;
                code_tree_.set_single(static_cast<u16>(code));
                return true;
            }

            if (static_cast<size_t>(n) > NUM_CODES) n = NUM_CODES;

            u8 lengths[NUM_CODES] = {0};
            int i = 0;

            while (i < n) {
                int code = temp_tree_.read(reader);
                if (code < 0) return false;

                if (code <= 2) {
                    int skip_count;
                    if (code == 0) {
                        skip_count = 1;
                    } else if (code == 1) {
                        int extra = reader.read_bits(4);
                        if (extra < 0) return false;
                        skip_count = extra + 3;
                    } else {
                        int extra = reader.read_bits(9);
                        if (extra < 0) return false;
                        skip_count = extra + 20;
                    }

                    for (int j = 0; j < skip_count && i < n; j++) {
                        lengths[i++] = 0;
                    }
                } else {
                    lengths[i++] = static_cast<u8>(code - 2);
                }
            }

            return code_tree_.build(lengths, static_cast <size_t>(n));
        }

        bool read_offset_table(bit_reader& reader) {
            int n = reader.read_bits(6);  // 6 bits for LK7
            if (n < 0) return false;

            if (n == 0) {
                int code = reader.read_bits(6);
                if (code < 0) return false;
                offset_tree_.set_single(static_cast<u16>(code));
                return true;
            }

            if (static_cast<size_t>(n) > MAX_OFFSET_CODES) n = MAX_OFFSET_CODES;

            u8 lengths[MAX_OFFSET_CODES] = {0};
            for (int i = 0; i < n; i++) {
                int len = read_length_value(reader);
                if (len < 0) return false;
                lengths[i] = static_cast<u8>(len);
            }

            return offset_tree_.build(lengths, static_cast <size_t>(n));
        }

        // LHark-specific offset decoding
        int read_offset(bit_reader& reader) const {
            int bits = offset_tree_.read(reader);
            if (bits < 0) return -1;

            if (bits < 4) return bits;

            // LHark offset expansion
            int num_low_bits = (bits - 2) / 2;
            int low_bits = reader.read_bits(static_cast <unsigned>(num_low_bits));
            if (low_bits < 0) return -1;

            return ((2 + (bits % 2)) << num_low_bits) + low_bits;
        }

        std::array<u8, RING_SIZE> ringbuf_{};
        size_t ringbuf_pos_ = 0;
        size_t block_remaining_ = 0;

        huffman_tree<MAX_TEMP_CODES> temp_tree_;
        huffman_tree<NUM_CODES> code_tree_;
        huffman_tree<MAX_OFFSET_CODES> offset_tree_;
    };

}  // namespace lha

struct lha_archive::impl {
    byte_vector data_;
    std::vector<lha::file_header> members_;
    std::vector<file_entry> files_;

    result_t<std::tuple<lha::file_header, size_t, size_t>> parse_header(size_t pos);
    result_t<std::tuple<lha::file_header, size_t, size_t>> parse_level01_header(size_t pos) const;
    result_t<std::tuple<lha::file_header, size_t, size_t>> parse_level2_header(size_t pos);
    result_t<std::tuple<lha::file_header, size_t, size_t>> parse_level3_header(size_t pos);
    void parse_extended_headers(lha::file_header& header, size_t start, size_t end);

    [[nodiscard]] u16 read_u16(size_t pos) const {
        return static_cast<u16>(data_[pos]) |
               (static_cast<u16>(data_[pos + 1]) << 8);
    }

    [[nodiscard]] u32 read_u32(size_t pos) const {
        return static_cast<u32>(data_[pos]) |
               (static_cast<u32>(data_[pos + 1]) << 8) |
               (static_cast<u32>(data_[pos + 2]) << 16) |
               (static_cast<u32>(data_[pos + 3]) << 24);
    }

    [[nodiscard]] u32 decode_dos_time(u32 dos_time) const {
        if (dos_time == 0) return 0;

        int sec = (dos_time & 0x1F) * 2;
        int min = (dos_time >> 5) & 0x3F;
        int hour = (dos_time >> 11) & 0x1F;
        int day = (dos_time >> 16) & 0x1F;
        int month = static_cast <int>((dos_time >> 21) & 0x0F) - 1;
        int year = 80 + static_cast <int>((dos_time >> 25) & 0x7F);

        struct tm tm = {};
        tm.tm_sec = sec;
        tm.tm_min = min;
        tm.tm_hour = hour;
        tm.tm_mday = day;
        tm.tm_mon = month;
        tm.tm_year = year;
        tm.tm_isdst = -1;

        return static_cast<u32>(mktime(&tm));
    }
};

lha_archive::lha_archive()
    : m_pimpl(std::make_unique <impl>()) {}

lha_archive::~lha_archive() = default;

result_t<std::unique_ptr<lha_archive>> lha_archive::open(byte_span data) {
    auto archive = std::unique_ptr<lha_archive>(new lha_archive());
    archive->m_pimpl->data_.assign(data.begin(), data.end());

    auto result = archive->parse();
    if (!result) return std::unexpected(result.error());

    return archive;
}

result_t<std::unique_ptr<lha_archive>> lha_archive::open(const std::filesystem::path& path) {
    auto file = file_input_stream::open(path);
    if (!file) return std::unexpected(file.error());

    auto size = file->size();
    if (!size) return std::unexpected(size.error());

    byte_vector data(*size);
    auto read = file->read(data);
    if (!read) return std::unexpected(read.error());

    return open(data);
}

const std::vector<file_entry>& lha_archive::files() const { return m_pimpl->files_; }

result_t<byte_vector> lha_archive::extract(const file_entry& entry) {
    if (entry.folder_index >= m_pimpl->members_.size()) {
        return std::unexpected(error{error_code::FileNotInArchive});
    }

    const auto& member = m_pimpl->members_[entry.folder_index];

    if (member.is_directory()) {
        return byte_vector{};
    }

    if (entry.folder_offset + member.compressed_size > m_pimpl->data_.size()) {
        return std::unexpected(error{error_code::TruncatedArchive});
    }

    byte_span compressed(m_pimpl->data_.data() + entry.folder_offset, member.compressed_size);

    // Decompress based on method
    result_t<byte_vector> output;

    if (std::memcmp(member.method, lha::METHOD_LH0, 5) == 0 ||
        std::memcmp(member.method, lha::METHOD_LZ4, 5) == 0 ||
        std::memcmp(member.method, lha::METHOD_PM0, 5) == 0) {
        // Stored
        output = byte_vector(compressed.begin(), compressed.end());
    }
    else if (std::memcmp(member.method, lha::METHOD_LH5, 5) == 0 ||
             std::memcmp(member.method, lha::METHOD_LH4, 5) == 0) {
        lha::lh5_decoder decoder;
        output = decoder.decompress(compressed, member.original_size);
    }
    else if (std::memcmp(member.method, lha::METHOD_LH6, 5) == 0) {
        lha::lh6_decoder decoder;
        output = decoder.decompress(compressed, member.original_size);
    }
    else if (std::memcmp(member.method, lha::METHOD_LH7, 5) == 0) {
        // Detect LHark format: level 1, OS type = ' ' (0x20)
        if (member.header_level == 1 && member.os_type == ' ') {
            lha::lk7_decoder decoder;
            output = decoder.decompress(compressed, member.original_size);
        } else {
            lha::lh7_decoder decoder;
            output = decoder.decompress(compressed, member.original_size);
        }
    }
    else if (std::memcmp(member.method, lha::METHOD_LZS, 5) == 0) {
        output = lha::lzs_decoder::decompress(compressed, member.original_size);
    }
    else if (std::memcmp(member.method, lha::METHOD_LZ5, 5) == 0) {
        output = lha::lz5_decoder::decompress(compressed, member.original_size);
    }
    else if (std::memcmp(member.method, lha::METHOD_LH1, 5) == 0) {
        lha::lh1_decoder decoder;
        output = decoder.decompress(compressed, member.original_size);
    }
    else {
        return std::unexpected(error{error_code::UnsupportedCompression,
            std::string("Unsupported LHA method: ") + member.method});
    }

    if (!output) return output;

    // Verify CRC-16
    u16 calc_crc = lha::crc_16::calculate(*output);
    if (calc_crc != member.crc) {
        return std::unexpected(error{error_code::InvalidChecksum, "LHA CRC-16 mismatch"});
    }

    // Report byte-level progress
    if (byte_progress_cb_) {
        byte_progress_cb_(entry, output->size(), output->size());
    }

    return output;
}

void_result_t lha_archive::parse() {
    size_t pos = 0;

    while (pos < m_pimpl->data_.size()) {
        auto header_result = m_pimpl->parse_header(pos);
        if (!header_result) {
            // Could be end of archive or invalid header
            if (header_result.error().code() == error_code::InvalidSignature) {
                break;  // End of archive
            }
            return std::unexpected(header_result.error());
        }

        const auto& [header, data_offset, next_pos] = *header_result;

        if (!header.is_directory()) {
            file_entry entry;
            entry.name = header.full_path();
            entry.uncompressed_size = header.original_size;
            entry.compressed_size = header.compressed_size;
            entry.folder_index = static_cast<u32>(m_pimpl->members_.size());
            entry.folder_offset = data_offset;

            m_pimpl->files_.push_back(entry);
        }

        m_pimpl->members_.push_back(header);
        pos = next_pos;
    }

    return {};
}

result_t<std::tuple<lha::file_header, size_t, size_t>> lha_archive::impl::parse_header(size_t pos) {
    if (pos + 22 > data_.size()) {
        return std::unexpected(error{error_code::InvalidSignature});
    }

    // Check for end marker
    if (data_[pos] == 0) {
        return std::unexpected(error{error_code::InvalidSignature});
    }

    // Detect header level (at offset 20)
    u8 header_level = data_[pos + 20];

    switch (header_level) {
        case 0:
        case 1:
            return parse_level01_header(pos);
        case 2:
            return parse_level2_header(pos);
        case 3:
            return parse_level3_header(pos);
        default:
            return std::unexpected(error{error_code::InvalidSignature, "Unknown LHA header level"});
    }
}

result_t<std::tuple<lha::file_header, size_t, size_t>> lha_archive::impl::parse_level01_header(size_t pos) const {
    u8 header_len = data_[pos];
    u8 checksum = data_[pos + 1];

    // Read full header
    size_t total_header_len = header_len + 2;
    if (pos + total_header_len > data_.size()) {
        return std::unexpected(error{error_code::TruncatedArchive});
    }

    // Verify checksum (sum of bytes from offset 2)
    u8 calc_sum = 0;
    for (size_t i = 2; i < total_header_len; i++) {
        calc_sum += data_[pos + i];
    }
    if (calc_sum != checksum) {
        return std::unexpected(error{error_code::InvalidChecksum, "LHA header checksum mismatch"});
    }

    lha::file_header header;
    header.header_level = data_[pos + 20];

    // Compression method
    std::memcpy(header.method, &data_[pos + 2], 5);
    header.method[5] = '\0';

    // Sizes
    header.compressed_size = read_u32(pos + 7);
    header.original_size = read_u32(pos + 11);

    // Timestamp (MS-DOS format)
    header.timestamp = decode_dos_time(read_u32(pos + 15));

    // Path length
    u8 path_len = data_[pos + 21];
    if (22 + path_len > header_len) {
        return std::unexpected(error{error_code::TruncatedArchive});
    }

    // Read filename
    header.filename.assign(reinterpret_cast<const char*>(&data_[pos + 22]), path_len);

    // Convert backslashes to forward slashes
    for (char& c : header.filename) {
        if (c == '\\') c = '/';
    }

    // Split path and filename
    auto sep_pos = header.filename.rfind('/');
    if (sep_pos != std::string::npos) {
        header.path = header.filename.substr(0, sep_pos + 1);
        header.filename = header.filename.substr(sep_pos + 1);
    }

    // CRC-16
    header.crc = read_u16(pos + 22 + path_len);

    // OS type (level 1 only)
    if (header.header_level == 1 && 24 + path_len <= header_len) {
        header.os_type = data_[pos + 24 + path_len];
    }

    size_t data_offset = pos + total_header_len;

    // Handle extended headers for level 1
    if (header.header_level == 1) {
        // First extended header size is in last 2 bytes of basic header
        u16 ext_size = read_u16(pos + total_header_len - 2);
        size_t ext_pos = pos + total_header_len;  // Extended headers start after basic header

        while (ext_size != 0) {
            if (ext_pos + ext_size > data_.size()) break;
            if (header.compressed_size < ext_size) break;
            header.compressed_size -= ext_size;

            // Extended header: type (1) + data (ext_size-3) + next_size (2)
            // Parse extended header if needed
            if (ext_size >= 3) {
                u8 ext_type = data_[ext_pos];
                // Could parse type-specific data here
                (void)ext_type;
            }

            // Move to end of this extended header
            ext_pos += ext_size;

            // Next size is in the last 2 bytes of this header
            if (ext_size >= 2) {
                ext_size = read_u16(ext_pos - 2);
            } else {
                break;
            }
        }

        data_offset = ext_pos;
    }

    size_t next_pos = data_offset + header.compressed_size;
    return std::make_tuple(header, data_offset, next_pos);
}

result_t<std::tuple<lha::file_header, size_t, size_t>> lha_archive::impl::parse_level2_header(size_t pos) {
    u16 header_len = read_u16(pos);

    if (header_len < 26 || pos + header_len > data_.size()) {
        return std::unexpected(error{error_code::TruncatedArchive});
    }

    lha::file_header header;
    header.header_level = 2;

    // Compression method
    std::memcpy(header.method, &data_[pos + 2], 5);
    header.method[5] = '\0';

    // Sizes
    header.compressed_size = read_u32(pos + 7);
    header.original_size = read_u32(pos + 11);

    // Unix timestamp
    header.timestamp = read_u32(pos + 15);

    // CRC-16
    header.crc = read_u16(pos + 21);

    // OS type
    header.os_type = data_[pos + 23];

    // LHA for OS-9/68k generates broken level 2 headers: the header length
    // field is the length of the remainder of the header, not the complete
    // header length. As a result it's two bytes too short.
    size_t actual_header_len = header_len;
    if (header.os_type == 'K') {  // OS-9/68k
        actual_header_len += 2;
        if (pos + actual_header_len > data_.size()) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }
    }

    // Parse extended headers
    parse_extended_headers(header, pos + 24, pos + actual_header_len);

    size_t data_offset = pos + actual_header_len;
    size_t next_pos = data_offset + header.compressed_size;

    return std::make_tuple(header, data_offset, next_pos);
}

result_t<std::tuple<lha::file_header, size_t, size_t>> lha_archive::impl::parse_level3_header(size_t pos) {
    if (pos + 32 > data_.size()) {
        return std::unexpected(error{error_code::TruncatedArchive});
    }

    u16 word_size = read_u16(pos);
    if (word_size != 4) {
        return std::unexpected(error{error_code::InvalidSignature, "Unsupported LHA level 3 word size"});
    }

    u32 header_len = read_u32(pos + 24);
    if (header_len < 32 || pos + header_len > data_.size()) {
        return std::unexpected(error{error_code::TruncatedArchive});
    }

    lha::file_header header;
    header.header_level = 3;

    // Compression method
    std::memcpy(header.method, &data_[pos + 2], 5);
    header.method[5] = '\0';

    // Sizes
    header.compressed_size = read_u32(pos + 7);
    header.original_size = read_u32(pos + 11);

    // Unix timestamp
    header.timestamp = read_u32(pos + 15);

    // CRC-16
    header.crc = read_u16(pos + 21);

    // OS type
    header.os_type = data_[pos + 23];

    // Parse extended headers
    parse_extended_headers(header, pos + 28, pos + header_len);

    size_t data_offset = pos + header_len;
    size_t next_pos = data_offset + header.compressed_size;

    return std::make_tuple(header, data_offset, next_pos);
}

void lha_archive::impl::parse_extended_headers(lha::file_header& header, size_t start, size_t end) {
    size_t pos = start;

    while (pos + 3 <= end) {
        u16 ext_size = read_u16(pos);
        if (ext_size == 0 || pos + ext_size > end) break;

        u8 ext_type = data_[pos + 2];

        switch (ext_type) {
            case 0x01:  // Filename
                if (ext_size > 3) {
                    header.filename.assign(
                        reinterpret_cast<const char*>(&data_[pos + 3]),
                        ext_size - 3);
                }
                break;
            case 0x02:  // Path
                if (ext_size > 3) {
                    header.path.assign(
                        reinterpret_cast<const char*>(&data_[pos + 3]),
                        ext_size - 3);
                    // Ensure trailing slash
                    if (!header.path.empty() && header.path.back() != '/') {
                        header.path += '/';
                    }
                }
                break;
            // Other extended headers can be added as needed
        }

        pos += ext_size;
    }
}

}  // namespace crate
