#include <crate/formats/zip.hh>
#include <crate/compression/inflate.hh>
#include <crate/core/crc.hh>
#include <crate/core/path.hh>
#include <cstring>
#include <fstream>

namespace crate {

namespace {
    // Maximum size for a single file extraction to prevent zip bomb attacks
    // 1GB is a reasonable limit for in-memory extraction
    constexpr size_t MAX_EXTRACTION_SIZE = 1ULL * 1024 * 1024 * 1024;
}

namespace zip {
    // Signatures
    constexpr u32 LOCAL_FILE_SIGNATURE = 0x04034b50;
    constexpr u32 CENTRAL_DIR_SIGNATURE = 0x02014b50;
    constexpr u32 END_OF_CENTRAL_DIR_SIGNATURE = 0x06054b50;
    constexpr u32 ZIP64_END_OF_CENTRAL_DIR_SIGNATURE = 0x06064b50;
    constexpr u32 ZIP64_END_OF_CENTRAL_DIR_LOCATOR_SIGNATURE = 0x07064b50;

    // Compression methods
    enum compression_method : u16 {
        STORED = 0,
        SHRUNK = 1,
        REDUCED_1 = 2,
        REDUCED_2 = 3,
        REDUCED_3 = 4,
        REDUCED_4 = 5,
        IMPLODED = 6,
        DEFLATED = 8,
        DEFLATE64 = 9,
        BZIP2 = 12,
        LZMA = 14,
        ZSTD = 93,
        XZ = 95,
        PPMD = 98
    };

    // General purpose bit flags
    enum flags : u16 {
        ENCRYPTED = 0x0001,
        DATA_DESCRIPTOR = 0x0008,
        UTF8_NAMES = 0x0800
    };

    struct local_file_header {
        u16 version_needed = 0;
        u16 flags = 0;
        u16 compression = 0;
        u16 mod_time = 0;
        u16 mod_date = 0;
        u32 crc32 = 0;
        u64 compressed_size = 0;
        u64 uncompressed_size = 0;
        u16 filename_len = 0;
        u16 extra_len = 0;
        std::string filename;
        size_t data_offset = 0;
    };

    struct central_dir_entry {
        u16 version_made_by = 0;
        u16 version_needed = 0;
        u16 flags = 0;
        u16 compression = 0;
        u16 mod_time = 0;
        u16 mod_date = 0;
        u32 crc32 = 0;
        u64 compressed_size = 0;
        u64 uncompressed_size = 0;
        u16 filename_len = 0;
        u16 extra_len = 0;
        u16 comment_len = 0;
        u32 disk_start = 0;
        u16 internal_attr = 0;
        u32 external_attr = 0;
        u64 local_header_offset = 0;
        std::string filename;
    };

    struct end_of_central_dir {
        u16 disk_num = 0;
        u16 central_dir_disk = 0;
        u64 entries_on_disk = 0;
        u64 total_entries = 0;
        u64 central_dir_size = 0;
        u64 central_dir_offset = 0;
        std::string comment;
    };
}

struct zip_archive::impl {
    byte_vector data_;
    std::vector<zip::central_dir_entry> entries_;
    std::vector<file_entry> files_;
    zip::end_of_central_dir eocd_;
};

zip_archive::zip_archive()
    : pimpl_(std::make_unique<impl>()) {
}

zip_archive::~zip_archive() = default;

result_t<std::unique_ptr<zip_archive>> zip_archive::open(byte_span data) {
    auto archive = std::unique_ptr<zip_archive>(new zip_archive());
    archive->pimpl_->data_.assign(data.begin(), data.end());

    auto result = archive->parse();
    if (!result) {
        return crate::make_unexpected(result.error());
    }

    return archive;
}

result_t<std::unique_ptr<zip_archive>> zip_archive::open(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return crate::make_unexpected(error{error_code::FileNotFound, "Cannot open file"});
    }

    file.seekg(0, std::ios::end);
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    byte_vector data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);

    return open(data);
}

const std::vector<file_entry>& zip_archive::files() const {
    return pimpl_->files_;
}

void_result_t zip_archive::parse() {
    const auto& data = pimpl_->data_;

    if (data.size() < 22) {
        return crate::make_unexpected(error{error_code::TruncatedArchive, "File too small for ZIP"});
    }

    // Find End of Central Directory record (search from end)
    size_t eocd_offset = 0;
    bool found = false;

    // EOCD must be within last 65557 bytes (22 byte EOCD + 65535 max comment)
    size_t search_start = data.size() > 65557 ? data.size() - 65557 : 0;

    for (size_t i = data.size() - 22; i >= search_start; --i) {
        if (read_u32_le(data.data() + i) == zip::END_OF_CENTRAL_DIR_SIGNATURE) {
            eocd_offset = i;
            found = true;
            break;
        }
        if (i == 0) break;
    }

    if (!found) {
        return crate::make_unexpected(error{error_code::InvalidSignature, "ZIP EOCD signature not found"});
    }

    // Parse EOCD
    size_t pos = eocd_offset + 4;
    pimpl_->eocd_.disk_num = read_u16_le(data.data() + pos); pos += 2;
    pimpl_->eocd_.central_dir_disk = read_u16_le(data.data() + pos); pos += 2;
    pimpl_->eocd_.entries_on_disk = read_u16_le(data.data() + pos); pos += 2;
    pimpl_->eocd_.total_entries = read_u16_le(data.data() + pos); pos += 2;
    pimpl_->eocd_.central_dir_size = read_u32_le(data.data() + pos); pos += 4;
    pimpl_->eocd_.central_dir_offset = read_u32_le(data.data() + pos); pos += 4;
    u16 comment_len = read_u16_le(data.data() + pos); pos += 2;

    if (pos + comment_len > data.size()) {
        return crate::make_unexpected(error{error_code::TruncatedArchive, "ZIP comment truncated"});
    }

    // Check for ZIP64
    if (pimpl_->eocd_.central_dir_offset == 0xFFFFFFFF ||
        pimpl_->eocd_.total_entries == 0xFFFF) {
        // Look for ZIP64 EOCD locator
        if (eocd_offset >= 20) {
            size_t locator_pos = eocd_offset - 20;
            if (read_u32_le(data.data() + locator_pos) == zip::ZIP64_END_OF_CENTRAL_DIR_LOCATOR_SIGNATURE) {
                // Parse ZIP64 locator
                u64 zip64_eocd_offset = read_u64_le(data.data() + locator_pos + 8);

                if (zip64_eocd_offset + 56 <= data.size() &&
                    read_u32_le(data.data() + zip64_eocd_offset) == zip::ZIP64_END_OF_CENTRAL_DIR_SIGNATURE) {
                    // Parse ZIP64 EOCD
                    pos = zip64_eocd_offset + 24;
                    pimpl_->eocd_.entries_on_disk = read_u64_le(data.data() + pos); pos += 8;
                    pimpl_->eocd_.total_entries = read_u64_le(data.data() + pos); pos += 8;
                    pimpl_->eocd_.central_dir_size = read_u64_le(data.data() + pos); pos += 8;
                    pimpl_->eocd_.central_dir_offset = read_u64_le(data.data() + pos);
                }
            }
        }
    }

    // Parse Central Directory
    pos = static_cast<size_t>(pimpl_->eocd_.central_dir_offset);

    for (u64 i = 0; i < pimpl_->eocd_.total_entries; ++i) {
        if (pos + 46 > data.size()) {
            return crate::make_unexpected(error{error_code::TruncatedArchive, "Central directory truncated"});
        }

        if (read_u32_le(data.data() + pos) != zip::CENTRAL_DIR_SIGNATURE) {
            return crate::make_unexpected(error{error_code::InvalidHeader, "Invalid central directory entry"});
        }

        zip::central_dir_entry entry;
        pos += 4;
        entry.version_made_by = read_u16_le(data.data() + pos); pos += 2;
        entry.version_needed = read_u16_le(data.data() + pos); pos += 2;
        entry.flags = read_u16_le(data.data() + pos); pos += 2;
        entry.compression = read_u16_le(data.data() + pos); pos += 2;
        entry.mod_time = read_u16_le(data.data() + pos); pos += 2;
        entry.mod_date = read_u16_le(data.data() + pos); pos += 2;
        entry.crc32 = read_u32_le(data.data() + pos); pos += 4;
        entry.compressed_size = read_u32_le(data.data() + pos); pos += 4;
        entry.uncompressed_size = read_u32_le(data.data() + pos); pos += 4;
        entry.filename_len = read_u16_le(data.data() + pos); pos += 2;
        entry.extra_len = read_u16_le(data.data() + pos); pos += 2;
        entry.comment_len = read_u16_le(data.data() + pos); pos += 2;
        entry.disk_start = read_u16_le(data.data() + pos); pos += 2;
        entry.internal_attr = read_u16_le(data.data() + pos); pos += 2;
        entry.external_attr = read_u32_le(data.data() + pos); pos += 4;
        entry.local_header_offset = read_u32_le(data.data() + pos); pos += 4;

        if (pos + entry.filename_len + entry.extra_len + entry.comment_len > data.size()) {
            return crate::make_unexpected(error{error_code::TruncatedArchive, "Central directory entry truncated"});
        }

        entry.filename.assign(reinterpret_cast<const char*>(data.data() + pos), entry.filename_len);
        pos += entry.filename_len;

        // Parse extra field for ZIP64 sizes
        size_t extra_end = pos + entry.extra_len;
        while (pos + 4 <= extra_end) {
            u16 header_id = read_u16_le(data.data() + pos);
            u16 data_size = read_u16_le(data.data() + pos + 2);
            pos += 4;

            // Bounds check: ensure data_size doesn't exceed remaining extra field
            if (pos + data_size > extra_end) {
                break;
            }

            if (header_id == 0x0001) {
                // ZIP64 extended information
                size_t field_pos = pos;
                if (entry.uncompressed_size == 0xFFFFFFFF && field_pos + 8 <= pos + data_size) {
                    entry.uncompressed_size = read_u64_le(data.data() + field_pos);
                    field_pos += 8;
                }
                if (entry.compressed_size == 0xFFFFFFFF && field_pos + 8 <= pos + data_size) {
                    entry.compressed_size = read_u64_le(data.data() + field_pos);
                    field_pos += 8;
                }
                if (entry.local_header_offset == 0xFFFFFFFF && field_pos + 8 <= pos + data_size) {
                    entry.local_header_offset = read_u64_le(data.data() + field_pos);
                }
            }
            pos += data_size;
        }
        pos = extra_end + entry.comment_len;

        // Create file_entry
        file_entry fe;
        fe.name = sanitize_path(entry.filename);
        fe.compressed_size = entry.compressed_size;
        fe.uncompressed_size = entry.uncompressed_size;
        fe.is_directory = !entry.filename.empty() && entry.filename.back() == '/';
        fe.is_encrypted = (entry.flags & zip::ENCRYPTED) != 0;

        // DOS datetime - store raw values (dos_date_time has decoder methods)
        fe.datetime.date = entry.mod_date;
        fe.datetime.time = entry.mod_time;

        // Store index for extraction
        fe.folder_index = static_cast<u32>(pimpl_->entries_.size());

        pimpl_->entries_.push_back(std::move(entry));
        pimpl_->files_.push_back(std::move(fe));
    }

    return {};
}

result_t<byte_vector> zip_archive::extract(const file_entry& entry) {
    if (entry.folder_index >= pimpl_->entries_.size()) {
        return crate::make_unexpected(error{error_code::InvalidParameter, "Invalid entry index"});
    }

    const auto& cd_entry = pimpl_->entries_[entry.folder_index];
    const auto& data = pimpl_->data_;

    if (cd_entry.flags & zip::ENCRYPTED) {
        return crate::make_unexpected(error{error_code::EncryptionError, "Encrypted ZIP not supported"});
    }

    // Read local file header to get actual data offset
    size_t pos = static_cast<size_t>(cd_entry.local_header_offset);

    if (pos + 30 > data.size()) {
        return crate::make_unexpected(error{error_code::TruncatedArchive, "Local header truncated"});
    }

    if (read_u32_le(data.data() + pos) != zip::LOCAL_FILE_SIGNATURE) {
        return crate::make_unexpected(error{error_code::InvalidHeader, "Invalid local file header"});
    }

    pos += 26;
    u16 local_filename_len = read_u16_le(data.data() + pos); pos += 2;
    u16 local_extra_len = read_u16_le(data.data() + pos); pos += 2;
    pos += local_filename_len + local_extra_len;

    size_t data_offset = pos;
    size_t compressed_size = static_cast<size_t>(cd_entry.compressed_size);
    size_t uncompressed_size = static_cast<size_t>(cd_entry.uncompressed_size);

    if (data_offset + compressed_size > data.size()) {
        return crate::make_unexpected(error{error_code::TruncatedArchive, "File data truncated"});
    }

    // Guard against zip bombs - reject unreasonably large allocations
    if (uncompressed_size > MAX_EXTRACTION_SIZE) {
        return crate::make_unexpected(error{error_code::AllocationLimitExceeded,
            "Uncompressed size exceeds maximum allowed (" + std::to_string(uncompressed_size) + " bytes)"});
    }

    byte_span compressed(data.data() + data_offset, compressed_size);
    byte_vector output(uncompressed_size);

    switch (cd_entry.compression) {
        case zip::STORED:
            if (compressed_size != uncompressed_size) {
                return crate::make_unexpected(error{error_code::CorruptData, "Size mismatch for stored file"});
            }
            std::memcpy(output.data(), compressed.data(), compressed_size);
            break;

        case zip::DEFLATED: {
            inflate_decompressor inflater;
            auto result = inflater.decompress(compressed, output);
            if (!result) {
                return crate::make_unexpected(result.error());
            }
            if (*result != uncompressed_size) {
                return crate::make_unexpected(error{error_code::CorruptData, "Decompressed size mismatch"});
            }
            break;
        }

        default:
            return crate::make_unexpected(error{error_code::UnsupportedCompression,
                "Unsupported ZIP compression method: " + std::to_string(cd_entry.compression)});
    }

    // Verify CRC32
    u32 calc_crc = eval_crc_32(output);
    if (calc_crc != cd_entry.crc32) {
        return crate::make_unexpected(error{error_code::InvalidChecksum, "ZIP CRC32 mismatch"});
    }

    if (byte_progress_cb_) {
        byte_progress_cb_(entry, output.size(), output.size());
    }

    return output;
}

} // namespace crate
