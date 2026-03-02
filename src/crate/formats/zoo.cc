#include <crate/formats/zoo.hh>
#include <crate/core/crc.hh>
#include <crate/compression/lzh.hh>
#include <crate/compression/zoo_lzw.hh>
#include <array>
#include <cstring>
#include <utility>

namespace crate {
    namespace zoo {
        constexpr u32 ZOO_TAG = 0xFDC4A7DC;
        constexpr size_t HEADER_TEXT_SIZE = 20;
        constexpr size_t HEADER_SIZE = 46; // 20 + 4 + 22
        constexpr size_t FNAMESIZE = 13;
        constexpr size_t DIRENT_SIZE = 59;

        enum method : u8 {
            STORED = 0,
            LZW = 1,
            LH5 = 2
        };

        struct archive_header {
            u32 zoo_start = 0; // Where directory entries begin
            u32 zoo_minus = 0; // Consistency check
            u8 major_ver = 0;
            u8 minor_ver = 0;
            u32 cmt_pos = 0;
            u32 cmt_len = 0;
            u32 vdata = 0;
        };

        struct dir_entry {
            u8 method = 0;
            u32 next = 0; // Position of next entry
            u32 offset = 0; // Position of file data
            dos_date_time datetime{};
            u16 crc16 = 0;
            u32 original_size = 0;
            u32 compressed_size = 0;
            std::string filename;
            bool deleted = false;
        };

    }

    struct zoo_archive::impl {
        byte_vector data_;
        zoo::archive_header header_;
        std::vector <zoo::dir_entry> members_;
        std::vector <file_entry> files_;
    };

    zoo_archive::zoo_archive()
        : m_pimpl(std::make_unique <impl>()) {}

    zoo_archive::~zoo_archive() = default;

    result_t <std::unique_ptr <zoo_archive>> zoo_archive::open(byte_span data) {
        auto archive = std::unique_ptr <zoo_archive>(new zoo_archive());
        archive->m_pimpl->data_.assign(data.begin(), data.end());

        auto result = archive->parse();
        if (!result) return crate::make_unexpected(result.error());

        return archive;
    }

    result_t <std::unique_ptr <zoo_archive>> zoo_archive::open(const std::filesystem::path& path) {
        auto file = file_input_stream::open(path);
        if (!file) return crate::make_unexpected(file.error());

        auto size = file->size();
        if (!size) return crate::make_unexpected(size.error());

        byte_vector data(*size);
        auto read = file->read(data);
        if (!read) return crate::make_unexpected(read.error());

        return open(data);
    }

    const std::vector <file_entry>& zoo_archive::files() const { return m_pimpl->files_; }

    result_t <byte_vector> zoo_archive::extract(const file_entry& entry) {
        vector_output_stream output(entry.uncompressed_size);
        auto result = extract_to(entry, output);
        if (!result) return crate::make_unexpected(result.error());
        return output.take();
    }

    result_t <size_t> zoo_archive::extract_to(const file_entry& entry, output_stream& dest) {
        struct crc_output_stream final : output_stream {
            explicit crc_output_stream(output_stream& dest_stream)
                : dest(dest_stream) {
            }

            void_result_t write(byte_span data) override {
                crc.update(data);
                return dest.write(data);
            }

            output_stream& dest;
            crc_16_ibm crc;
        };

        if (entry.folder_index >= m_pimpl->members_.size()) {
            return crate::make_unexpected(error{error_code::FileNotInArchive});
        }

        const auto& member = m_pimpl->members_[entry.folder_index];
        if (member.offset + member.compressed_size > m_pimpl->data_.size()) {
            return crate::make_unexpected(error{error_code::TruncatedArchive});
        }

        byte_span compressed(m_pimpl->data_.data() + member.offset, member.compressed_size);
        memory_input_stream input(compressed);
        crc_output_stream crc_dest(dest);
        size_t written = 0;

        switch (member.method) {
            case zoo::STORED: {
                auto write = crc_dest.write(compressed);
                if (!write) return crate::make_unexpected(write.error());
                written = compressed.size();
                break;
            }

            case zoo::LZW: {
                zoo_lzw_decompressor decomp;
                decomp.set_expected_output_size(member.original_size);
                auto result = decomp.decompress_stream(input, crc_dest, member.original_size);
                if (!result) return crate::make_unexpected(result.error());
                written = *result;
                break;
            }

            case zoo::LH5: {
                lzh_decompressor decomp(lzh_format::LH5);
                auto result = decomp.decompress_stream(input, crc_dest, member.original_size);
                if (!result) return crate::make_unexpected(result.error());
                written = *result;
                break;
            }

            default:
                return crate::make_unexpected(error{
                    error_code::UnsupportedCompression,
                    "Unsupported ZOO compression method"
                });
        }

        // Verify CRC (using CRC-16-IBM)
        if (crc_dest.crc.finalize() != member.crc16) {
            return crate::make_unexpected(error{error_code::InvalidChecksum, "CRC-16 mismatch"});
        }

        // Report byte-level progress
        if (byte_progress_cb_) {
            byte_progress_cb_(entry, written, member.original_size);
        }

        return written;
    }

    void_result_t zoo_archive::parse() {
        // Check minimum size for header
        if (m_pimpl->data_.size() < zoo::HEADER_SIZE) {
            return crate::make_unexpected(error{error_code::TruncatedArchive});
        }

        // Check signature "ZOO 2.10 Archive."
        const char* expected = "ZOO 2.10 Archive.";
        if (std::memcmp(m_pimpl->data_.data(), expected, 17) != 0) {
            return crate::make_unexpected(error{error_code::InvalidSignature, "Not a ZOO archive"});
        }

        // Read header
        size_t pos = zoo::HEADER_TEXT_SIZE;

        u32 tag = read_u32_le(m_pimpl->data_.data() + pos);
        if (tag != zoo::ZOO_TAG) {
            return crate::make_unexpected(error{error_code::InvalidSignature, "Invalid ZOO tag"});
        }
        pos += 4;

        m_pimpl->header_.zoo_start = read_u32_le(m_pimpl->data_.data() + pos);
        pos += 4;
        m_pimpl->header_.zoo_minus = read_u32_le(m_pimpl->data_.data() + pos);
        pos += 4;
        m_pimpl->header_.major_ver = m_pimpl->data_[pos++];
        m_pimpl->header_.minor_ver = m_pimpl->data_[pos++];
        m_pimpl->header_.cmt_pos = read_u32_le(m_pimpl->data_.data() + pos);
        pos += 4;
        m_pimpl->header_.cmt_len = read_u32_le(m_pimpl->data_.data() + pos);
        pos += 4;
        m_pimpl->header_.vdata = read_u32_le(m_pimpl->data_.data() + pos);

        // Navigate to first directory entry
        pos = m_pimpl->header_.zoo_start;

        while (pos + zoo::DIRENT_SIZE <= m_pimpl->data_.size()) {
            // Read directory entry
            tag = read_u32_le(m_pimpl->data_.data() + pos);
            if (tag != zoo::ZOO_TAG) break;
            pos += 4;

            pos++; // dir_type (skip)

            zoo::dir_entry entry;
            entry.method = m_pimpl->data_[pos++];
            entry.next = read_u32_le(m_pimpl->data_.data() + pos);
            pos += 4;
            entry.offset = read_u32_le(m_pimpl->data_.data() + pos);
            pos += 4;

            u32 datetime = read_u32_le(m_pimpl->data_.data() + pos);
            entry.datetime.date = static_cast <u16>(datetime & 0xFFFF);
            entry.datetime.time = static_cast <u16>((datetime >> 16) & 0xFFFF);
            pos += 4;

            entry.crc16 = read_u16_le(m_pimpl->data_.data() + pos);
            pos += 2;
            entry.original_size = read_u32_le(m_pimpl->data_.data() + pos);
            pos += 4;
            entry.compressed_size = read_u32_le(m_pimpl->data_.data() + pos);
            pos += 4;

            u8 major_ver = m_pimpl->data_[pos++];
            u8 minor_ver = m_pimpl->data_[pos++];
            (void)major_ver;
            (void)minor_ver;

            u8 deleted = m_pimpl->data_[pos++];
            entry.deleted = (deleted != 0);
            pos++; // struc

            pos += 4; // comment
            pos += 2; // cmt_size

            // Read filename (13 bytes, null-terminated)
            std::string filename;
            size_t name_start = pos;
            while (pos < m_pimpl->data_.size() && pos < name_start + zoo::FNAMESIZE && m_pimpl->data_[pos] != 0) {
                filename += static_cast <char>(m_pimpl->data_[pos]);
                pos++;
            }
            pos = name_start + zoo::FNAMESIZE;
            entry.filename = filename;

            // Skip variable part
            pos += 8; // var_dir_len, tz, dir_crc, namlen, dirlen

            if (!entry.deleted) {
                file_entry file_entry;
                file_entry.name = entry.filename;
                file_entry.uncompressed_size = entry.original_size;
                file_entry.compressed_size = entry.compressed_size;
                file_entry.datetime = entry.datetime;
                file_entry.folder_index = static_cast <u32>(m_pimpl->members_.size());
                file_entry.folder_offset = entry.offset;

                m_pimpl->files_.push_back(file_entry);
                m_pimpl->members_.push_back(entry);
            }

            // Navigate to next entry
            if (entry.next == 0) break;
            pos = entry.next;
        }

        return {};
    }
} // namespace crate
