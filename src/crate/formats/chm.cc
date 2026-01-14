#include <crate/formats/chm.hh>
#include <crate/compression/lzx.hh>

namespace crate {
    namespace chm {
        constexpr u8 ITSF_SIGNATURE[] = {'I', 'T', 'S', 'F'};
        constexpr u8 ITSP_SIGNATURE[] = {'I', 'T', 'S', 'P'};
        constexpr u8 PMGL_SIGNATURE[] = {'P', 'M', 'G', 'L'};
        constexpr u8 PMGI_SIGNATURE[] = {'P', 'M', 'G', 'I'};

        struct itsf_header {
            u32 version = 0;
            u32 header_len = 0;
            u32 unknown1 = 0;
            u32 timestamp = 0;
            u32 language_id = 0;
            u64 section0_offset = 0;
            u64 section0_length = 0;
            u64 section1_offset = 0;
            u64 section1_length = 0;
        };

        struct itsp_header {
            u32 version = 0;
            u32 header_len = 0;
            u32 unknown1 = 0;
            u32 chunk_size = 0;
            u32 density = 0;
            u32 depth = 0;
            u32 root_chunk = 0;
            u32 first_pmgl = 0;
            u32 last_pmgl = 0;
            u32 unknown2 = 0;
            u32 dir_chunks = 0;
        };

        struct lzxc_header {
            u32 version = 0;
            u32 reset_interval = 0;
            u32 window_size = 0;
            u32 cache_size = 0;
        };
    }

    struct chm_archive::impl {
        byte_vector data_;
        chm::itsf_header itsf_{};
        chm::itsp_header itsp_{};
        chm::lzxc_header lzxc_{};

        std::vector <u64> reset_table_;
        std::vector <file_entry> files_;

        // Internal file entry with additional CHM-specific data
        struct internal_entry {
            std::string name;
            u64 offset = 0;
            u64 length = 0;
            u32 section = 0;
        };

        std::vector <internal_entry> internal_entries_;
    };

    result_t <std::unique_ptr <chm_archive>> chm_archive::open(byte_span data) {
        auto archive = std::unique_ptr <chm_archive>(new chm_archive());
        archive->m_pimpl->data_.assign(data.begin(), data.end());

        auto result = archive->parse();
        if (!result) return std::unexpected(result.error());

        return archive;
    }

    result_t <std::unique_ptr <chm_archive>> chm_archive::open(const std::filesystem::path& path) {
        auto file = file_input_stream::open(path);
        if (!file) return std::unexpected(file.error());

        auto size = file->size();
        if (!size) return std::unexpected(size.error());

        byte_vector data(*size);
        auto read = file->read(data);
        if (!read) return std::unexpected(read.error());

        return open(data);
    }

    const std::vector <file_entry>& chm_archive::files() const { return m_pimpl->files_; }

    result_t <byte_vector> chm_archive::extract(const file_entry& entry) {
        // Find internal entry
        for (const auto& ie : m_pimpl->internal_entries_) {
            if (!ie.name.empty() && ie.name.substr(1) == entry.name) {
                if (ie.section == 0) {
                    // Uncompressed section
                    if (m_pimpl->itsf_.section0_offset + ie.offset + ie.length > m_pimpl->data_.size()) {
                        return std::unexpected(error{error_code::TruncatedArchive});
                    }

                    byte_vector result(ie.length);
                    std::copy_n(
                        m_pimpl->data_.begin() + static_cast <std::ptrdiff_t>(
                            m_pimpl->itsf_.section0_offset + ie.offset),
                        ie.length, result.begin());
                    if (byte_progress_cb_) {
                        byte_progress_cb_(entry, result.size(), result.size());
                    }
                    return result;
                } else {
                    // Compressed section - requires full LZX decompression
                    return std::unexpected(error{
                        error_code::UnsupportedCompression,
                        "CHM LZX decompression not fully implemented"
                    });
                }
            }
        }

        return std::unexpected(error{error_code::FileNotInArchive});
    }

    void_result_t chm_archive::extract(const file_entry& entry, const std::filesystem::path& dest) {
        auto data = extract(entry);
        if (!data) return std::unexpected(data.error());

        auto output = file_output_stream::create(dest);
        if (!output) return std::unexpected(output.error());

        return output->write(*data);
    }

    void_result_t chm_archive::extract_all(const std::filesystem::path& dest_dir) {
        for (size_t i = 0; i < m_pimpl->files_.size(); i++) {
            const auto& entry = m_pimpl->files_[i];

            if (progress_cb_) {
                progress_cb_(entry, i + 1, m_pimpl->files_.size());
            }

            auto dest = dest_dir / entry.name;
            auto result = extract(entry, dest);
            if (!result) {
                // Skip files that can't be extracted (compressed content)
                continue;
            }
        }

        return {};
    }

    chm_archive::chm_archive()
        : m_pimpl(std::make_unique <impl>()) {
    }

    chm_archive::~chm_archive() = default;

    void_result_t chm_archive::parse() {
        auto result = parse_itsf_header();
        if (!result) return result;

        result = parse_directory();
        if (!result) return result;

        // Reset table is optional
        parse_reset_table();

        return {};
    }

    void_result_t chm_archive::parse_itsf_header() {
        // Check signature first (only need 4 bytes)
        if (m_pimpl->data_.size() < 4 ||
            std::memcmp(m_pimpl->data_.data(), chm::ITSF_SIGNATURE, 4) != 0) {
            return std::unexpected(error{
                error_code::InvalidSignature,
                "Not a valid CHM file"
            });
        }

        if (m_pimpl->data_.size() < 56) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        const u8* p = m_pimpl->data_.data() + 4;
        m_pimpl->itsf_.version = read_u32_le(p);
        p += 4;
        m_pimpl->itsf_.header_len = read_u32_le(p);
        p += 4;
        m_pimpl->itsf_.unknown1 = read_u32_le(p);
        p += 4;
        m_pimpl->itsf_.timestamp = read_u32_le(p);
        p += 4;
        m_pimpl->itsf_.language_id = read_u32_le(p);
        p += 4;
        p += 32; // GUIDs
        m_pimpl->itsf_.section0_offset = read_u64_le(p);
        p += 8;
        m_pimpl->itsf_.section0_length = read_u64_le(p);
        p += 8;
        m_pimpl->itsf_.section1_offset = read_u64_le(p);
        p += 8;
        m_pimpl->itsf_.section1_length = read_u64_le(p);

        return {};
    }

    result_t <u64> chm_archive::read_encint(byte_span data, size_t& pos) {
        u64 result = 0;
        u8 value = 0;
        do {
            if (pos >= data.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }
            value = data[pos++];
            result = (result << 7) | (value & 0x7F);
        }
        while (value & 0x80);
        return result;
    }

    void_result_t chm_archive::parse_directory() {
        auto section1_start = static_cast <size_t>(m_pimpl->itsf_.section1_offset);
        if (section1_start + 84 > m_pimpl->data_.size()) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        // Parse ITSP header
        const u8* p = m_pimpl->data_.data() + section1_start;
        if (std::memcmp(p, chm::ITSP_SIGNATURE, 4) != 0) {
            return std::unexpected(error{
                error_code::InvalidHeader,
                "Invalid ITSP header"
            });
        }
        p += 4;

        m_pimpl->itsp_.version = read_u32_le(p);
        p += 4;
        m_pimpl->itsp_.header_len = read_u32_le(p);
        p += 4;
        m_pimpl->itsp_.unknown1 = read_u32_le(p);
        p += 4;
        m_pimpl->itsp_.chunk_size = read_u32_le(p);
        p += 4;
        m_pimpl->itsp_.density = read_u32_le(p);
        p += 4;
        m_pimpl->itsp_.depth = read_u32_le(p);
        p += 4;
        m_pimpl->itsp_.root_chunk = read_u32_le(p);
        p += 4;
        m_pimpl->itsp_.first_pmgl = read_u32_le(p);
        p += 4;
        m_pimpl->itsp_.last_pmgl = read_u32_le(p);
        p += 4;
        p += 4; // unknown
        m_pimpl->itsp_.dir_chunks = read_u32_le(p);

        // Parse directory chunks (PMGL)
        size_t chunk_offset = section1_start + m_pimpl->itsp_.header_len;

        for (u32 chunk = m_pimpl->itsp_.first_pmgl;
             chunk <= m_pimpl->itsp_.last_pmgl && chunk < m_pimpl->itsp_.dir_chunks;) {
            if (chunk_offset + m_pimpl->itsp_.chunk_size > m_pimpl->data_.size()) {
                break;
            }

            byte_span chunk_data(m_pimpl->data_.data() + chunk_offset, m_pimpl->itsp_.chunk_size);

            if (std::memcmp(chunk_data.data(), chm::PMGL_SIGNATURE, 4) == 0) {
                // Parse PMGL chunk
                size_t pos = 20; // Skip header
                size_t quickref_size = read_u16_le(
                    chunk_data.data() + m_pimpl->itsp_.chunk_size - 2);
                size_t entries_end = m_pimpl->itsp_.chunk_size - quickref_size - 2;

                while (pos < entries_end) {
                    // Read entry name
                    auto name_len = read_encint(chunk_data, pos);
                    if (!name_len) return std::unexpected(name_len.error());

                    if (pos + *name_len > entries_end) break;

                    std::string name(reinterpret_cast <const char*>(chunk_data.data() + pos),
                                     static_cast <size_t>(*name_len));
                    pos += static_cast <size_t>(*name_len);

                    // Read section
                    auto section = read_encint(chunk_data, pos);
                    if (!section) return std::unexpected(section.error());

                    // Read offset
                    auto offset = read_encint(chunk_data, pos);
                    if (!offset) return std::unexpected(offset.error());

                    // Read length
                    auto length = read_encint(chunk_data, pos);
                    if (!length) return std::unexpected(length.error());

                    impl::internal_entry entry;
                    entry.name = name;
                    entry.section = static_cast <u32>(*section);
                    entry.offset = *offset;
                    entry.length = *length;
                    m_pimpl->internal_entries_.push_back(entry);

                    // Also add to public file list (skip meta files and directories)
                    if (entry.length > 0 && !name.empty() &&
                        name[0] == '/' && name.back() != '/') {
                        file_entry fe;
                        fe.name = name.substr(1); // Remove leading '/'
                        fe.uncompressed_size = entry.length;
                        fe.compressed_size = 0;
                        fe.folder_index = entry.section;
                        fe.folder_offset = entry.offset;
                        m_pimpl->files_.push_back(fe);
                    }
                }

                // Move to next chunk
                i32 next = *reinterpret_cast <const i32*>(chunk_data.data() + 16);
                if (next < 0) break;
                chunk = static_cast <u32>(next);
                chunk_offset = section1_start + m_pimpl->itsp_.header_len +
                               static_cast <size_t>(chunk) * m_pimpl->itsp_.chunk_size;
            } else {
                chunk++;
                chunk_offset += m_pimpl->itsp_.chunk_size;
            }
        }

        return {};
    }

    void_result_t chm_archive::parse_reset_table() {
        // Find ::DataSpace/Storage/MSCompressed/Transform/{...}/InstanceData/ResetTable
        for (const auto& entry : m_pimpl->internal_entries_) {
            if (entry.name.find("ResetTable") != std::string::npos) {
                // Found reset table - parse for LZX reset intervals
                break;
            }
        }
        return {};
    }
} // namespace crate::chm
