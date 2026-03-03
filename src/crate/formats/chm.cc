#include <crate/formats/chm.hh>
#include <crate/compression/lzx.hh>
#include <crate/core/path.hh>

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

        // Base offset for section 0 content (comes after section 1)
        u64 content_base_offset_ = 0;

        // Compressed content info
        u64 compressed_offset_ = 0;
        u64 compressed_length_ = 0;
        u64 uncompressed_length_ = 0;
        unsigned lzx_window_bits_ = 15;

        // Reset table for LZX decompression
        std::vector <u64> reset_table_;
        u64 reset_block_size_ = 0;

        // Cached decompressed content (lazily populated)
        byte_vector decompressed_content_;

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
        if (!result) return crate::make_unexpected(result.error());

        return archive;
    }

    result_t <std::unique_ptr <chm_archive>> chm_archive::open(const std::filesystem::path& path) {
        auto file = file_input_stream::open(path);
        if (!file) return crate::make_unexpected(file.error());

        auto size = file->size();
        if (!size) return crate::make_unexpected(size.error());

        byte_vector data(*size);
        auto read = file->read(data);
        if (!read) return crate::make_unexpected(read.error());

        return open(data);
    }

    result_t <std::unique_ptr <chm_archive>> chm_archive::open(std::istream& stream) {
        auto data = read_stream(stream);
        if (!data) return crate::make_unexpected(data.error());
        return open(*data);
    }

    const std::vector <file_entry>& chm_archive::files() const { return m_pimpl->files_; }

    void_result_t chm_archive::decompress_content() {
        if (!m_pimpl->decompressed_content_.empty()) {
            return {};
        }

        if (m_pimpl->compressed_length_ == 0 || m_pimpl->uncompressed_length_ == 0) {
            return crate::make_unexpected(error{
                error_code::UnsupportedCompression,
                "CHM missing compression metadata"
            });
        }

        auto dec = lzx_decompressor::create(m_pimpl->lzx_window_bits_, lzx_mode::chm);
        if (!dec) {
            return crate::make_unexpected(dec.error());
        }

        size_t comp_total = static_cast<size_t>(m_pimpl->compressed_length_);
        size_t uncomp_total = static_cast<size_t>(m_pimpl->uncompressed_length_);

        if (m_pimpl->compressed_offset_ + comp_total > m_pimpl->data_.size()) {
            return crate::make_unexpected(error{error_code::TruncatedArchive,
                "Compressed content extends beyond archive data"});
        }

        constexpr size_t MAX_UNCOMPRESSED = 1ULL << 30; // 1 GB
        if (uncomp_total > MAX_UNCOMPRESSED) {
            return crate::make_unexpected(error{error_code::AllocationLimitExceeded,
                "Uncompressed size exceeds maximum allowed"});
        }

        const u8* comp_base = m_pimpl->data_.data() + m_pimpl->compressed_offset_;
        m_pimpl->decompressed_content_.resize(uncomp_total);

        auto& reset_table = m_pimpl->reset_table_;

        if (reset_table.empty()) {
            // No reset table — decompress everything in one pass
            (*dec)->set_expected_output_size(uncomp_total);
            auto result = (*dec)->decompress_some(
                byte_span(comp_base, comp_total),
                m_pimpl->decompressed_content_,
                true);
            if (!result) {
                m_pimpl->decompressed_content_.clear();
                return crate::make_unexpected(result.error());
            }
            return {};
        }

        // LZXC reset_interval: number of 32K frames between decoder resets
        // Version 2: raw value is already in frame units
        // Version 1: raw value is in bytes, divide by 32768
        unsigned reset_interval_frames = m_pimpl->lzxc_.reset_interval;
        if (m_pimpl->lzxc_.version == 1 && reset_interval_frames >= 32768) {
            reset_interval_frames /= 32768;
        }
        if (reset_interval_frames == 0) reset_interval_frames = 1;

        // Each reset table entry covers one 32K frame.
        // Reset intervals span multiple frames (e.g., 2 entries per reset).
        // Decompress one reset interval at a time.
        size_t output_pos = 0;
        size_t entry_idx = 0;

        while (entry_idx < reset_table.size() && output_pos < uncomp_total) {
            // This reset interval starts at entry_idx and covers reset_interval_frames entries
            size_t interval_start = entry_idx;
            size_t interval_end = std::min(entry_idx + reset_interval_frames, reset_table.size());

            // Compressed data range for this reset interval
            auto comp_start_off = static_cast<size_t>(reset_table[interval_start]);
            auto comp_end_off = (interval_end < reset_table.size())
                ? static_cast<size_t>(reset_table[interval_end])
                : comp_total;

            if (comp_start_off >= comp_total) break;
            if (comp_end_off > comp_total) comp_end_off = comp_total;
            if (comp_end_off <= comp_start_off) break;

            const u8* interval_ptr = comp_base + comp_start_off;
            size_t interval_len = comp_end_off - comp_start_off;

            // Output size: frames * 32768, clamped to remaining
            size_t interval_output = std::min(
                static_cast<size_t>(reset_interval_frames) * 32768,
                uncomp_total - output_pos);

            if (interval_start > 0) {
                (*dec)->reset_at_interval();
            }
            (*dec)->set_expected_output_size(interval_output);

            mutable_byte_span output_span(
                m_pimpl->decompressed_content_.data() + output_pos,
                interval_output);

            auto result = (*dec)->decompress_some(
                byte_span(interval_ptr, interval_len),
                output_span,
                true);

            if (!result) {
                m_pimpl->decompressed_content_.clear();
                return crate::make_unexpected(result.error());
            }

            output_pos += result->bytes_written;
            entry_idx = interval_end;
        }

        if (output_pos < uncomp_total) {
            m_pimpl->decompressed_content_.clear();
            return crate::make_unexpected(error{
                error_code::CorruptData,
                "LZX decompression incomplete: wrote " + std::to_string(output_pos) +
                " of " + std::to_string(uncomp_total) + " bytes"
            });
        }

        return {};
    }

    result_t <byte_vector> chm_archive::extract(const file_entry& entry) {
        // Find internal entry
        for (const auto& ie : m_pimpl->internal_entries_) {
            if (!ie.name.empty() && ie.name.substr(1) == entry.name) {
                if (ie.section == 0) {
                    // Uncompressed section - content starts after section 1 (directory)
                    u64 file_offset = m_pimpl->content_base_offset_ + ie.offset;
                    if (file_offset + ie.length > m_pimpl->data_.size()) {
                        return crate::make_unexpected(error{error_code::TruncatedArchive});
                    }

                    byte_vector result(ie.length);
                    std::copy_n(
                        m_pimpl->data_.begin() + static_cast <std::ptrdiff_t>(file_offset),
                        ie.length, result.begin());
                    if (byte_progress_cb_) {
                        byte_progress_cb_(entry, result.size(), result.size());
                    }
                    return result;
                } else {
                    // Compressed section - decompress if needed
                    auto decomp_result = decompress_content();
                    if (!decomp_result) {
                        return crate::make_unexpected(decomp_result.error());
                    }

                    // Extract from decompressed content
                    if (ie.offset + ie.length > m_pimpl->decompressed_content_.size()) {
                        return crate::make_unexpected(error{error_code::TruncatedArchive});
                    }

                    byte_vector result(ie.length);
                    std::copy_n(
                        m_pimpl->decompressed_content_.begin() + static_cast<std::ptrdiff_t>(ie.offset),
                        ie.length, result.begin());
                    if (byte_progress_cb_) {
                        byte_progress_cb_(entry, result.size(), result.size());
                    }
                    return result;
                }
            }
        }

        return crate::make_unexpected(error{error_code::FileNotInArchive});
    }

    void_result_t chm_archive::extract(const file_entry& entry, const std::filesystem::path& dest) {
        auto data = extract(entry);
        if (!data) return crate::make_unexpected(data.error());

        auto output = file_output_stream::create(dest);
        if (!output) return crate::make_unexpected(output.error());

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
            if (!result) return result;
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
            return crate::make_unexpected(error{
                error_code::InvalidSignature,
                "Not a valid CHM file"
            });
        }

        if (m_pimpl->data_.size() < 56) {
            return crate::make_unexpected(error{error_code::TruncatedArchive});
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

        // Section 0 content starts after section 1 (the directory)
        m_pimpl->content_base_offset_ = m_pimpl->itsf_.section1_offset +
                                        m_pimpl->itsf_.section1_length;

        return {};
    }

    result_t <u64> chm_archive::read_encint(byte_span data, size_t& pos) {
        u64 result = 0;
        u8 value = 0;
        do {
            if (pos >= data.size()) {
                return crate::make_unexpected(error{error_code::TruncatedArchive});
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
            return crate::make_unexpected(error{error_code::TruncatedArchive});
        }

        // Parse ITSP header
        const u8* p = m_pimpl->data_.data() + section1_start;
        if (std::memcmp(p, chm::ITSP_SIGNATURE, 4) != 0) {
            return crate::make_unexpected(error{
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
                    if (!name_len) return crate::make_unexpected(name_len.error());

                    if (pos + *name_len > entries_end) break;

                    std::string name(reinterpret_cast <const char*>(chunk_data.data() + pos),
                                     static_cast <size_t>(*name_len));
                    pos += static_cast <size_t>(*name_len);

                    // Read section
                    auto section = read_encint(chunk_data, pos);
                    if (!section) return crate::make_unexpected(section.error());

                    // Read offset
                    auto offset = read_encint(chunk_data, pos);
                    if (!offset) return crate::make_unexpected(offset.error());

                    // Read length
                    auto length = read_encint(chunk_data, pos);
                    if (!length) return crate::make_unexpected(length.error());

                    impl::internal_entry entry;
                    entry.name = name;
                    entry.section = static_cast <u32>(*section);
                    entry.offset = *offset;
                    entry.length = *length;
                    m_pimpl->internal_entries_.push_back(entry);

                    // Also add to public file list (skip meta files and directories)
                    if (entry.length > 0 && !name.empty() &&
                        name[0] == '/' && name.back() != '/') {
                        auto safe_name = sanitize_path(name.substr(1));
                        if (!safe_name.empty()) {
                            file_entry fe;
                            fe.name = std::move(safe_name);
                            fe.uncompressed_size = entry.length;
                            fe.compressed_size = 0;
                            fe.folder_index = entry.section;
                            fe.folder_offset = entry.offset;
                            m_pimpl->files_.push_back(fe);
                        }
                    }
                }

                // Move to next chunk
                i32 next = static_cast<i32>(read_u32_le(chunk_data.data() + 16));
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
        // Find and parse compression metadata from section 0 entries
        for (const auto& entry : m_pimpl->internal_entries_) {
            if (entry.section != 0) continue;

            u64 file_offset = m_pimpl->content_base_offset_ + entry.offset;
            if (file_offset + entry.length > m_pimpl->data_.size()) continue;

            const u8* p = m_pimpl->data_.data() + file_offset;

            if (entry.name.find("ControlData") != std::string::npos && entry.length >= 28) {
                // Parse LZXC header
                // Format: size(4), "LZXC"(4), version(4), reset_interval(4), window_size(4), cache_size(4)
                u32 lzxc_size = read_u32_le(p);
                if (lzxc_size >= 6 && std::memcmp(p + 4, "LZXC", 4) == 0) {
                    m_pimpl->lzxc_.version = read_u32_le(p + 8);
                    m_pimpl->lzxc_.reset_interval = read_u32_le(p + 12);
                    m_pimpl->lzxc_.window_size = read_u32_le(p + 16);
                    m_pimpl->lzxc_.cache_size = read_u32_le(p + 20);

                    // Convert window size to window bits
                    // Version 2: window_size is in 32KB units
                    u32 ws = (m_pimpl->lzxc_.version == 2)
                        ? m_pimpl->lzxc_.window_size * 0x8000
                        : m_pimpl->lzxc_.window_size;
                    for (unsigned i = 15; i <= 21; i++) {
                        if ((1u << i) == ws) {
                            m_pimpl->lzx_window_bits_ = i;
                            break;
                        }
                    }
                }
            } else if (entry.name.find("ResetTable") != std::string::npos && entry.length >= 40) {
                // Parse reset table
                // Format: version(4), num_entries(4), entry_size(4), header_len(4),
                //         uncompressed_len(8), compressed_len(8), block_len(8), entries[]
                u32 num_entries = read_u32_le(p + 4);
                u32 entry_size = read_u32_le(p + 8);
                u32 header_len = read_u32_le(p + 12);
                m_pimpl->uncompressed_length_ = read_u64_le(p + 16);
                m_pimpl->compressed_length_ = read_u64_le(p + 24);
                m_pimpl->reset_block_size_ = read_u64_le(p + 32);

                // Read reset table entries
                m_pimpl->reset_table_.clear();
                if (entry_size >= 8 && header_len <= entry.length) {
                    auto max_entries = static_cast<u32>((entry.length - header_len) / entry_size);
                    if (num_entries > max_entries) num_entries = max_entries;
                    for (u32 i = 0; i < num_entries; i++) {
                        u64 offset = read_u64_le(p + header_len + i * entry_size);
                        m_pimpl->reset_table_.push_back(offset);
                    }
                }
            } else if (entry.name.find("Content") != std::string::npos &&
                       entry.name.find("MSCompressed") != std::string::npos) {
                // Store compressed content location
                m_pimpl->compressed_offset_ = file_offset;
                // Note: Don't overwrite compressed_length_ here - use the value from ResetTable
                // entry.length may be larger (allocated space) vs actual compressed size
            }
        }
        return {};
    }
} // namespace crate::chm
