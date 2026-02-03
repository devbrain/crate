#include <crate/formats/arj.hh>
#include <crate/compression/arj_lz.hh>
#include <crate/compression/lzh.hh>
#include <crate/core/crc.hh>
#include <crate/core/path.hh>
#include <cstring>

namespace crate {

    namespace {
        // Maximum size for a single file extraction to prevent zip bomb attacks
        // 1GB is a reasonable limit for in-memory extraction
        constexpr size_t MAX_EXTRACTION_SIZE = 1ULL * 1024 * 1024 * 1024;
    }

    namespace arj {
        constexpr u8 SIGNATURE[] = {0x60, 0xEA};
        constexpr size_t MIN_HEADER_SIZE = 30;
        constexpr size_t MAX_HEADER_SIZE = 2600;

        // File types
        enum file_type : u8 {
            BINARY = 0,
            TEXT = 1,
            MAIN_HEADER = 2,
            DIRECTORY = 3,
            VOLUME_LABEL = 4,
            CHAPTER_LABEL = 5
        };

        // Compression methods
        enum method : u8 {
            STORED = 0,
            METHOD_1 = 1, // LZH (LH6)
            METHOD_2 = 2, // LZH (LH6)
            METHOD_3 = 3, // LZH (LH6)
            METHOD_4 = 4 // Custom LZ77
        };

        // Host OS
        enum host_os : u8 {
            OS_DOS = 0,
            OS_PRIMOS = 1,
            OS_UNIX = 2,
            OS_AMIGA = 3,
            OS_MACOS = 4,
            OS_OS2 = 5,
            OS_APPLEIIGS = 6,
            OS_ATARIST = 7,
            OS_NEXT = 8,
            OS_VMS = 9,
            OS_WIN95 = 10,
            OS_WIN32 = 11
        };

        // Flags
        enum flags : u8 {
            GARBLED = 0x01,
            ANSIPAGE = 0x02, // or OLD_SECURED for older versions
            VOLUME = 0x04,
            EXTFILE = 0x08, // or ARJPROT for main header
            PATHSYM = 0x10,
            SECURED = 0x40, // main header only
            ALTNAME = 0x80 // main header only
        };

        struct  main_header {
            u8 archiver_version = 0;
            u8 min_version = 0;
            u8 host_os = 0;
            u8 flags = 0;
            u8 security_version = 0;
            dos_date_time created{};
            dos_date_time modified{};
            u32 archive_size = 0;
            u32 security_pos = 0;
            u16 filespec_pos = 0;
            u16 security_len = 0;
            std::string filename;
            std::string comment;
        };

        struct  member_header {
            u8 archiver_version = 0;
            u8 min_version = 0;
            u8 host_os = 0;
            u8 flags = 0;
            u8 method = 0;
            u8 file_type = 0;
            dos_date_time modified{};
            u32 compressed_size = 0;
            u32 original_size = 0;
            u32 crc = 0;
            u16 filespec_pos = 0;
            u16 file_mode = 0;
            u8 first_chapter = 0;
            u8 last_chapter = 0;
            std::string filename;
            std::string comment;
        };
    }
    struct arj_archive::impl {
        byte_vector data_;
        arj::main_header main_header_;
        std::vector <arj::member_header> members_;
        std::vector <file_entry> files_;
    };

    result_t <std::unique_ptr <arj_archive>> arj_archive::open(byte_span data) {
        auto archive = std::unique_ptr <arj_archive>(new arj_archive());
        archive->m_pimpl->data_.assign(data.begin(), data.end());

        auto result = archive->parse();
        if (!result) return std::unexpected(result.error());

        return archive;
    }

    result_t <std::unique_ptr <arj_archive>> arj_archive::open(const std::filesystem::path& path) {
        auto file = file_input_stream::open(path);
        if (!file) return std::unexpected(file.error());

        auto size = file->size();
        if (!size) return std::unexpected(size.error());

        byte_vector data(*size);
        auto read = file->read(data);
        if (!read) return std::unexpected(read.error());

        return open(data);
    }

    const std::vector <file_entry>& arj_archive::files() const { return m_pimpl->files_; }

    result_t <byte_vector> arj_archive::extract(const file_entry& entry) {
        // Guard against zip bombs - reject unreasonably large allocations
        if (entry.uncompressed_size > MAX_EXTRACTION_SIZE) {
            return std::unexpected(error{error_code::AllocationLimitExceeded,
                "Uncompressed size exceeds maximum allowed (" + std::to_string(entry.uncompressed_size) + " bytes)"});
        }

        vector_output_stream output(entry.uncompressed_size);
        auto result = extract_to(entry, output);
        if (!result) return std::unexpected(result.error());
        return output.take();
    }

    result_t <size_t> arj_archive::extract_to(const file_entry& entry, output_stream& dest) {
        struct crc_output_stream final : output_stream {
            explicit crc_output_stream(output_stream& dest_stream)
                : dest(dest_stream) {
            }

            void_result_t write(byte_span data) override {
                crc.update(data);
                return dest.write(data);
            }

            output_stream& dest;
            crc_32 crc;
        };

        // Find the member by index
        if (entry.folder_index >= m_pimpl->members_.size()) {
            return std::unexpected(error{error_code::FileNotInArchive});
        }

        const auto& member = m_pimpl->members_[entry.folder_index];

        if (member.file_type == arj::DIRECTORY) {
            return 0;
        }

        if (member.flags & arj::GARBLED) {
            return std::unexpected(error{
                error_code::UnsupportedCompression,
                "Encrypted ARJ files are not supported"
            });
        }

        // Get compressed data
        if (entry.folder_offset + member.compressed_size > m_pimpl->data_.size()) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        byte_span compressed(m_pimpl->data_.data() + entry.folder_offset, member.compressed_size);
        memory_input_stream input(compressed);
        crc_output_stream crc_dest(dest);
        size_t written = 0;

        switch (member.method) {
            case arj::STORED: {
                if (compressed.size() != member.original_size) {
                    return std::unexpected(error{
                        error_code::CorruptData,
                        "Stored data size mismatch"
                    });
                }
                auto write = crc_dest.write(compressed);
                if (!write) return std::unexpected(write.error());
                written = compressed.size();
                break;
            }

            case arj::METHOD_1:
            case arj::METHOD_2:
            case arj::METHOD_3: {
                // LZH (LH6 or LH7)
                lzh_format fmt = (member.min_version == 51) ? lzh_format::LH7 : lzh_format::LH6;
                lzh_decompressor decompressor(fmt);
                auto result = decompressor.decompress_stream(input, crc_dest, member.original_size);
                if (!result) return std::unexpected(result.error());
                written = *result;
                break;
            }

            case arj::METHOD_4: {
                // Custom LZ77
                bool old_format = (member.archiver_version == 1);
                arj_method4_decompressor decompressor(old_format);
                auto result = decompressor.decompress_stream(input, crc_dest, member.original_size);
                if (!result) return std::unexpected(result.error());
                written = *result;
                break;
            }

            default:
                return std::unexpected(error{
                    error_code::UnsupportedCompression,
                    "Unknown ARJ compression method"
                });
        }

        // Verify CRC
        if (crc_dest.crc.finalize() != member.crc) {
            return std::unexpected(error{
                error_code::InvalidChecksum,
                "CRC mismatch"
            });
        }

        // Report byte-level progress
        if (byte_progress_cb_) {
            byte_progress_cb_(entry, written, member.original_size);
        }

        return written;
    }

   // const arj::main_header& arj_archive::main_header() const { return m_pimpl->main_header_; }

   // const std::vector <arj::member_header>& arj_archive::members() const { return m_pimpl->members_; }

    arj_archive::arj_archive()
        :m_pimpl(std::make_unique<impl>()) {}

    arj_archive::~arj_archive() = default;

    void_result_t arj_archive::parse() {
        size_t pos = 0;

        // Find and parse main header
        auto main_result = find_and_parse_main_header(pos);
        if (!main_result) return std::unexpected(main_result.error());
        pos = *main_result;

        // Parse member headers
        while (pos + 2 <= m_pimpl->data_.size()) {
            auto member_result = parse_member(pos);
            if (!member_result) {
                if (member_result.error().code() == error_code::TruncatedArchive) {
                    break; // End of archive
                }
                return std::unexpected(member_result.error());
            }
            pos = *member_result;
        }

        return {};
    }

    result_t <size_t> arj_archive::find_and_parse_main_header(size_t pos) {
        // Check signature
        if (pos + 2 > m_pimpl->data_.size() ||
            std::memcmp(m_pimpl->data_.data() + pos, arj::SIGNATURE, 2) != 0) {
            return std::unexpected(error{
                error_code::InvalidSignature,
                "Not a valid ARJ file"
            });
        }
        pos += 2;

        // Read basic header size
        if (pos + 2 > m_pimpl->data_.size()) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }
        u16 basic_header_size = read_u16_le(m_pimpl->data_.data() + pos);
        pos += 2;

        if (basic_header_size == 0) {
            return std::unexpected(error{
                error_code::InvalidHeader,
                "Invalid main header"
            });
        }

        if (basic_header_size < arj::MIN_HEADER_SIZE ||
            basic_header_size > arj::MAX_HEADER_SIZE) {
            return std::unexpected(error{
                error_code::InvalidHeader,
                "Invalid header size"
            });
        }

        if (pos + basic_header_size + 4 > m_pimpl->data_.size()) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        // Parse header contents
        const u8* hdr = m_pimpl->data_.data() + pos;
        size_t hdr_pos = 0;

        u8 first_hdr_size = hdr[hdr_pos++];
        m_pimpl->main_header_.archiver_version = hdr[hdr_pos++];
        m_pimpl->main_header_.min_version = hdr[hdr_pos++];
        m_pimpl->main_header_.host_os = hdr[hdr_pos++];
        m_pimpl->main_header_.flags = hdr[hdr_pos++];
        m_pimpl->main_header_.security_version = hdr[hdr_pos++];
        u8 file_type = hdr[hdr_pos++];
        hdr_pos++; // reserved

        if (file_type != arj::MAIN_HEADER) {
            return std::unexpected(error{
                error_code::InvalidHeader,
                "Expected main header"
            });
        }

        // Timestamps
        m_pimpl->main_header_.created.time = read_u16_le(hdr + hdr_pos);
        m_pimpl->main_header_.created.date = read_u16_le(hdr + hdr_pos + 2);
        hdr_pos += 4;
        m_pimpl->main_header_.modified.time = read_u16_le(hdr + hdr_pos);
        m_pimpl->main_header_.modified.date = read_u16_le(hdr + hdr_pos + 2);
        hdr_pos += 4;

        m_pimpl->main_header_.archive_size = read_u32_le(hdr + hdr_pos);
        hdr_pos += 4;
        m_pimpl->main_header_.security_pos = read_u32_le(hdr + hdr_pos);
        hdr_pos += 4;
        m_pimpl->main_header_.filespec_pos = read_u16_le(hdr + hdr_pos);
        hdr_pos += 2;
        m_pimpl->main_header_.security_len = read_u16_le(hdr + hdr_pos);
        //hdr_pos += 2;

        // Skip to filename (at first_hdr_size)
        hdr_pos = first_hdr_size;

        // Read filename (null-terminated)
        const size_t name_start = hdr_pos;
        while (hdr_pos < basic_header_size && hdr[hdr_pos] != 0) hdr_pos++;
        m_pimpl->main_header_.filename = std::string(reinterpret_cast <const char*>(hdr + name_start),
                                            hdr_pos - name_start);
        if (hdr_pos < basic_header_size) hdr_pos++;

        // Read comment (null-terminated)
        size_t comment_start = hdr_pos;
        while (hdr_pos < basic_header_size && hdr[hdr_pos] != 0) hdr_pos++;
        m_pimpl->main_header_.comment = std::string(reinterpret_cast <const char*>(hdr + comment_start),
                                           hdr_pos - comment_start);

        pos += basic_header_size;

        // Verify CRC
        u32 crc_reported = read_u32_le(m_pimpl->data_.data() + pos);
        pos += 4;

        u32 crc_calc = eval_crc_32(byte_span(m_pimpl->data_.data() + pos - basic_header_size - 4, basic_header_size));
        if (crc_calc != crc_reported) {
            // Warning only - don't fail
        }

        // Skip extended headers
        auto ext_result = skip_extended_headers(pos);
        if (!ext_result) return std::unexpected(ext_result.error());
        pos = *ext_result;

        return pos;
    }

    result_t <size_t> arj_archive::parse_member(size_t pos) {
        // Check signature
        if (pos + 2 > m_pimpl->data_.size()) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }
        if (std::memcmp(m_pimpl->data_.data() + pos, arj::SIGNATURE, 2) != 0) {
            return std::unexpected(error{
                error_code::InvalidSignature,
                "Invalid member signature"
            });
        }
        pos += 2;

        // Read basic header size
        if (pos + 2 > m_pimpl->data_.size()) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }
        u16 basic_header_size = read_u16_le(m_pimpl->data_.data() + pos);
        pos += 2;

        // End of archive marker
        if (basic_header_size == 0) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        if (pos + basic_header_size + 4 > m_pimpl->data_.size()) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        // Parse header contents
        const u8* hdr = m_pimpl->data_.data() + pos;
        size_t hdr_pos = 0;

        arj::member_header member;
        u8 first_hdr_size = hdr[hdr_pos++];
        member.archiver_version = hdr[hdr_pos++];
        member.min_version = hdr[hdr_pos++];
        member.host_os = hdr[hdr_pos++];
        member.flags = hdr[hdr_pos++];
        member.method = hdr[hdr_pos++];
        member.file_type = hdr[hdr_pos++];
        hdr_pos++; // reserved

        // Timestamp
        dos_date_time dt{};
        dt.time = read_u16_le(hdr + hdr_pos);
        dt.date = read_u16_le(hdr + hdr_pos + 2);
        member.modified = dt;
        hdr_pos += 4;

        member.compressed_size = read_u32_le(hdr + hdr_pos);
        hdr_pos += 4;
        member.original_size = read_u32_le(hdr + hdr_pos);
        hdr_pos += 4;
        member.crc = read_u32_le(hdr + hdr_pos);
        hdr_pos += 4;
        member.filespec_pos = read_u16_le(hdr + hdr_pos);
        hdr_pos += 2;
        member.file_mode = read_u16_le(hdr + hdr_pos);
        hdr_pos += 2;
        member.first_chapter = hdr[hdr_pos++];
        member.last_chapter = hdr[hdr_pos++];

        // Skip to filename
        hdr_pos = first_hdr_size;

        // Read filename (null-terminated)
        size_t name_start = hdr_pos;
        while (hdr_pos < basic_header_size && hdr[hdr_pos] != 0) hdr_pos++;
        member.filename = std::string(reinterpret_cast <const char*>(hdr + name_start),
                                      hdr_pos - name_start);
        if (hdr_pos < basic_header_size) hdr_pos++;

        // Read comment (null-terminated)
        size_t comment_start = hdr_pos;
        while (hdr_pos < basic_header_size && hdr[hdr_pos] != 0) hdr_pos++;
        member.comment = std::string(reinterpret_cast <const char*>(hdr + comment_start),
                                     hdr_pos - comment_start);

        pos += basic_header_size;

        // Skip CRC
        pos += 4;

        // Skip extended headers
        auto ext_result = skip_extended_headers(pos);
        if (!ext_result) return std::unexpected(ext_result.error());
        pos = *ext_result;

        // Calculate data position
        size_t data_pos = pos;

        // Create file entry
        file_entry entry;
        // Sanitize filename (convert backslashes, remove path traversal)
        entry.name = sanitize_path(member.filename);
        entry.uncompressed_size = member.original_size;
        entry.compressed_size = member.compressed_size;
        entry.datetime = member.modified;
        entry.folder_index = static_cast <u32>(m_pimpl->members_.size());
        entry.folder_offset = data_pos;

        // Set attributes
        if (member.host_os == arj::OS_DOS || member.host_os == arj::OS_OS2 ||
            member.host_os == arj::OS_WIN95 || member.host_os == arj::OS_WIN32) {
            entry.attribs.readonly = member.file_mode & 0x01;
            entry.attribs.hidden = member.file_mode & 0x02;
            entry.attribs.system = member.file_mode & 0x04;
            entry.attribs.archive = member.file_mode & 0x20;
        }

        m_pimpl->files_.push_back(entry);
        m_pimpl->members_.push_back(member);

        pos += member.compressed_size;
        return pos;
    }

    result_t <size_t> arj_archive::skip_extended_headers(size_t pos) const {
        while (true) {
            if (pos + 2 > m_pimpl->data_.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }

            u16 ext_size = read_u16_le(m_pimpl->data_.data() + pos);
            pos += 2;

            if (ext_size == 0) break;

            if (pos + ext_size + 4 > m_pimpl->data_.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }

            pos += ext_size + 4; // data + CRC
        }
        return pos;
    }
} // namespace crate::arj
