#include <crate/formats/arc.hh>
#include <crate/formats/arc_internal.hh>
#include <crate/core/crc.hh>
#include <cstring>
#include <array>

namespace crate {

    namespace arc {
        constexpr u8 HEADER_ID = 0x1A; // Header identifier byte
        constexpr size_t MAX_FILENAME_LEN = 13;

        enum method : u8 {
            END_OF_ARCHIVE = 0,
            UNPACKED_OLD = 1,
            UNPACKED = 2,
            PACKED = 3, // RLE90 only
            SQUEEZED = 4, // RLE + Huffman
            CRUNCHED_5 = 5, // LZW (12-bit)
            CRUNCHED_6 = 6, // LZW (12-bit)
            CRUNCHED_7 = 7, // LZW (12-bit)
            CRUNCHED = 8, // LZW (12-bit) + RLE
            SQUASHED = 9, // LZW (13-bit), no RLE
            CRUSHED = 10, // Not supported
            DISTILLED = 11 // Not supported
        };

        struct  member_header {
            u8 method = 0;
            std::string filename;
            u32 compressed_size = 0;
            u32 original_size = 0;
            dos_date_time datetime{};
            u16 crc16 = 0;
        };

        // Squeezed (Huffman) decompression
        inline result_t <byte_vector> unsqueeze(byte_span input) {
            if (input.size() < 2) {
                return std::unexpected(error{error_code::CorruptData, "Squeezed data too short"});
            }

            // Read number of nodes
            u16 num_nodes = read_u16_le(input.data());
            if (num_nodes > 257) {
                return std::unexpected(error{error_code::CorruptData, "Invalid Huffman tree"});
            }

            size_t pos = 2;
            if (pos + num_nodes * 4 > input.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }

            // Read Huffman tree (array of node pairs)
            std::vector <std::array <i16, 2>> tree(num_nodes);
            for (u16 i = 0; i < num_nodes; i++) {
                tree[i][0] = *reinterpret_cast <const i16*>(input.data() + pos);
                tree[i][1] = *reinterpret_cast <const i16*>(input.data() + pos + 2);
                pos += 4;
            }

            // Decode using bit-by-bit traversal
            byte_vector output;
            size_t bit_pos = 0;
            const u8* data = input.data() + pos;
            size_t data_len = input.size() - pos;

            constexpr i16 SPEOF = 256;

            while (true) {
                i16 node = 0;
                while (node >= 0) {
                    if (bit_pos / 8 >= data_len) break;
                    u8 value = data[bit_pos / 8];
                    int bit = (value >> (bit_pos % 8)) & 1;
                    bit_pos++;
                    if (node < 0 || static_cast <size_t>(node) >= tree.size()) {
                        return output;
                    }
                    node = tree[static_cast <size_t>(node)][static_cast <size_t>(bit)];
                }

                if (node == -(SPEOF + 1)) break; // End of file
                if (node < -256) break; // Invalid

                output.push_back(static_cast <u8>(-(node + 1)));
            }

            // Apply RLE decompression
            return unpack_rle(output);
        }

        // LZW decompression
        class  lzw_decoder {
            public:
                lzw_decoder(bool squashed = false)
                    : squashed_(squashed) {
                }

                result_t <byte_vector> decompress(byte_span input) {
                    if (input.empty()) return byte_vector{};

                    size_t pos = 0;

                    // For crunched (not squashed), first byte is bits info
                    if (!squashed_) {
                        if (input.size() < 1) return byte_vector{};
                        pos = 1; // Skip header byte
                    }

                    max_bits_ = squashed_ ? 13 : 12;
                    init();

                    byte_vector output;
                    std::vector <u8> stack;
                    stack.reserve(4096);

                    bit_pos_ = 0;
                    data_ = input.data() + pos;
                    data_len_ = input.size() - pos;

                    // Read first code
                    int code = read_code();
                    if (code < 0) return output;

                    u8 finchar = static_cast <u8>(code);
                    output.push_back(finchar);
                    int oldcode = code;

                    while (true) {
                        code = read_code();
                        if (code < 0) break;

                        if (code == CLEAR) {
                            init();
                            code = read_code();
                            if (code < 0) break;
                            finchar = static_cast <u8>(code);
                            output.push_back(finchar);
                            oldcode = code;
                            continue;
                        }

                        int incode = code;

                        // Special case: code not yet in table
                        if (code >= free_ent_) {
                            stack.push_back(finchar);
                            code = oldcode;
                        }

                        // Generate output string in reverse
                        while (code >= 256) {
                            if (code < 0 || code > MAX_CODE) {
                                return output;
                            }
                            stack.push_back(suffix_[static_cast <size_t>(code)]);
                            code = prefix_[static_cast <size_t>(code)];
                        }

                        finchar = static_cast <u8>(code);
                        stack.push_back(finchar);

                        // Output in correct order
                        while (!stack.empty()) {
                            output.push_back(stack.back());
                            stack.pop_back();
                        }

                        // Add to dictionary
                        if (free_ent_ < MAX_CODE) {
                            prefix_[static_cast <size_t>(free_ent_)] = static_cast <u16>(oldcode);
                            suffix_[static_cast <size_t>(free_ent_)] = finchar;
                            free_ent_++;

                            // Increase bit width if needed
                            if (free_ent_ > max_code_ && n_bits_ < max_bits_) {
                                n_bits_++;
                                max_code_ = (1 << n_bits_) - 1;
                            }
                        }

                        oldcode = incode;
                    }

                    if (!squashed_) {
                        return unpack_rle(output);
                    }
                    return output;
                }

            private:
                static constexpr int INIT_BITS = 9;
                static constexpr int CLEAR = 256;
                static constexpr int FIRST = 257;
                static constexpr int MAX_CODE = 8191;

                void init() {
                    n_bits_ = INIT_BITS;
                    max_code_ = (1 << n_bits_) - 1;
                    free_ent_ = FIRST;

                    // Initialize suffix table with identity
                    for (int i = 0; i < 256; i++) {
                        suffix_[static_cast <size_t>(i)] = static_cast <u8>(i);
                    }
                }

                int read_code() {
                    if (bit_pos_ / 8 >= data_len_) return -1;

                    // Read n_bits_ bits, LSB first
                    int code = 0;
                    for (int i = 0; i < n_bits_; i++) {
                        size_t byte_idx = bit_pos_ / 8;
                        if (byte_idx >= data_len_) return -1;
                        int bit = (data_[byte_idx] >> (bit_pos_ % 8)) & 1;
                        code |= (bit << i);
                        bit_pos_++;
                    }
                    return code;
                }

                bool squashed_ = false;
                int max_bits_ = 0;
                int n_bits_ = 0;
                int max_code_ = 0;
                int free_ent_ = 0;
                size_t bit_pos_ = 0;
                const u8* data_ = nullptr;
                size_t data_len_ = 0;
                std::array <u16, MAX_CODE + 1> prefix_{};
                std::array <u8, MAX_CODE + 1> suffix_{};
        };
    }

    struct arc_archive::impl {
        byte_vector data_;
        std::vector <arc::member_header> members_;
        std::vector <file_entry> files_;
    };

    result_t <std::unique_ptr <arc_archive>> arc_archive::open(byte_span data) {
        auto archive = std::unique_ptr <arc_archive>(new arc_archive());
        archive->m_pimpl->data_.assign(data.begin(), data.end());

        auto result = archive->parse();
        if (!result) return std::unexpected(result.error());

        return archive;
    }

    result_t <std::unique_ptr <arc_archive>> arc_archive::open(const std::filesystem::path& path) {
        auto file = file_input_stream::open(path);
        if (!file) return std::unexpected(file.error());

        auto size = file->size();
        if (!size) return std::unexpected(size.error());

        byte_vector data(*size);
        auto read = file->read(data);
        if (!read) return std::unexpected(read.error());

        return open(data);
    }

    const std::vector <file_entry>& arc_archive::files() const { return m_pimpl->files_; }

    result_t <byte_vector> arc_archive::extract(const file_entry& entry) {
        if (entry.folder_index >= m_pimpl->members_.size()) {
            return std::unexpected(error{error_code::FileNotInArchive});
        }

        const auto& member = m_pimpl->members_[entry.folder_index];
        byte_span compressed(m_pimpl->data_.data() + entry.folder_offset, member.compressed_size);

        byte_vector output;

        switch (member.method) {
            case arc::UNPACKED_OLD:
            case arc::UNPACKED:
                output.assign(compressed.begin(), compressed.end());
                break;

            case arc::PACKED:
                output = arc::unpack_rle(compressed);
                break;

            case arc::SQUEEZED: {
                auto result = arc::unsqueeze(compressed);
                if (!result) return std::unexpected(result.error());
                output = std::move(*result);
                break;
            }

            case arc::CRUNCHED_5:
            case arc::CRUNCHED_6:
            case arc::CRUNCHED_7:
            case arc::CRUNCHED: {
                arc::lzw_decoder decoder(false);
                auto result = decoder.decompress(compressed);
                if (!result) return std::unexpected(result.error());
                output = std::move(*result);
                break;
            }

            case arc::SQUASHED: {
                arc::lzw_decoder decoder(true);
                auto result = decoder.decompress(compressed);
                if (!result) return std::unexpected(result.error());
                output = std::move(*result);
                break;
            }

            default:
                return std::unexpected(error{
                    error_code::UnsupportedCompression,
                    "Unsupported ARC compression method"
                });
        }

        // Verify CRC (using CRC-16-IBM)
        u16 crc = eval_crc16_ibm(output);
        if (crc != member.crc16) {
            return std::unexpected(error{error_code::InvalidChecksum, "CRC-16 mismatch"});
        }

        // Report byte-level progress
        if (byte_progress_cb_) {
            byte_progress_cb_(entry, output.size(), output.size());
        }

        return output;
    }

    // const std::vector <arc::member_header>& arc_archive::members() const { return m_pimpl->members_; }

    arc_archive::arc_archive()
        : m_pimpl(std::make_unique<impl>()) {}

    arc_archive::~arc_archive() = default;

    void_result_t arc_archive::parse() {
        // ARC header format (28 bytes after ID byte):
        // - compression_method (1 byte)
        // - name (13 bytes, null-terminated)
        // - compressed_size (4 bytes)
        // - date_time (4 bytes)
        // - crc16 (2 bytes)
        // - original_size (4 bytes)
        constexpr size_t HEADER_SIZE = 28;

        size_t pos = 0;

        while (pos + 1 < m_pimpl->data_.size()) {
            // Look for header ID
            if (m_pimpl->data_[pos] != arc::HEADER_ID) {
                pos++;
                continue;
            }
            pos++; // Skip ID byte

            // Check method byte first (it's always present after ID)
            if (pos >= m_pimpl->data_.size()) break;
            u8 method = m_pimpl->data_[pos];
            if (method == arc::END_OF_ARCHIVE) break;
            pos++;

            // Now need at least the rest of the header
            if (pos + HEADER_SIZE - 1 > m_pimpl->data_.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }

            // Read filename (null-terminated, max 13 bytes)
            std::string filename;
            size_t name_start = pos;
            while (pos < m_pimpl->data_.size() && pos < name_start + arc::MAX_FILENAME_LEN && m_pimpl->data_[pos] != 0) {
                filename += static_cast <char>(m_pimpl->data_[pos]);
                pos++;
            }
            pos = name_start + arc::MAX_FILENAME_LEN; // Skip to fixed position

            arc::member_header member;
            member.method = method;
            member.filename = filename;
            member.compressed_size = read_u32_le(m_pimpl->data_.data() + pos);
            pos += 4;

            member.datetime.date = read_u16_le(m_pimpl->data_.data() + pos);
            member.datetime.time = read_u16_le(m_pimpl->data_.data() + pos + 2);
            pos += 4;

            member.crc16 = read_u16_le(m_pimpl->data_.data() + pos);
            pos += 2;

            // original_size is always present in the header
            member.original_size = read_u32_le(m_pimpl->data_.data() + pos);
            pos += 4;

            file_entry entry;
            entry.name = member.filename;
            entry.uncompressed_size = member.original_size;
            entry.compressed_size = member.compressed_size;
            entry.datetime = member.datetime;
            entry.folder_index = static_cast <u32>(m_pimpl->members_.size());
            entry.folder_offset = pos;

            m_pimpl->files_.push_back(entry);
            m_pimpl->members_.push_back(member);

            pos += member.compressed_size;
        }

        return {};
    }
} // namespace crate::arc
