#include <crate/formats/cab.hh>
#include <crate/core/lru_cache.hh>
#include <crate/core/path.hh>
#include <crate/compression/mszip.hh>
#include <crate/compression/lzx.hh>
#include <crate/compression/quantum.hh>
#include <cstring>

namespace crate {
    // CAB file header structures
    namespace cab {
        constexpr u8 SIGNATURE[] = {'M', 'S', 'C', 'F'};
        constexpr size_t HEADER_SIZE = 36;
        constexpr size_t FOLDER_SIZE = 8;
        constexpr size_t FILE_HEADER_SIZE = 16;
        constexpr size_t DATA_HEADER_SIZE = 8;

        // Header flags
        enum flags : u16 {
            PREV_CABINET = 0x0001,
            NEXT_CABINET = 0x0002,
            RESERVE_PRESENT = 0x0004
        };

        struct header {
            u32 cabinet_size = 0;
            u32 files_offset = 0;
            u8 version_minor = 0;
            u8 version_major = 0;
            u16 num_folders = 0;
            u16 num_files = 0;
            u16 flags = 0;
            u16 set_id = 0;
            u16 cabinet_index = 0;
            u16 header_reserve_size = 0;
            u8 folder_reserve_size = 0;
            u8 data_reserve_size = 0;
        };

        struct folder {
            u32 data_offset = 0;
            u16 num_blocks = 0;
            u16 comp_type = 0;
        };

        struct data_block {
            u32 checksum = 0;
            u16 compressed_size = 0;
            u16 uncompressed_size = 0;
        };
    }

    // PIMPL implementation struct for cab_archive
    struct cab_archive::impl {
        static constexpr size_t DEFAULT_FOLDER_CACHE_SIZE = 4;
        byte_vector data_;
        cab::header header_{};
        std::vector <cab::folder> folders_;
        std::vector <file_entry> files_;
        lru_cache <u32, byte_vector> folder_cache{DEFAULT_FOLDER_CACHE_SIZE};
    };

    cab_archive::cab_archive()
        : pimpl_(std::make_unique <impl>()) {
    }

    result_t <std::unique_ptr <cab_archive>> cab_archive::open(byte_span data) {
        auto archive = std::unique_ptr <cab_archive>(new cab_archive());
        archive->pimpl_->data_.assign(data.begin(), data.end());

        auto result = archive->parse();
        if (!result) return std::unexpected(result.error());

        return archive;
    }

    result_t <std::unique_ptr <cab_archive>> cab_archive::open(const std::filesystem::path& path) {
        auto file = file_input_stream::open(path);
        if (!file) return std::unexpected(file.error());

        auto size = file->size();
        if (!size) return std::unexpected(size.error());

        byte_vector data(*size);
        auto read = file->read(data);
        if (!read) return std::unexpected(read.error());

        return open(data);
    }

    const std::vector <file_entry>& cab_archive::files() const { return pimpl_->files_; }

    result_t <byte_vector> cab_archive::extract(const file_entry& entry) {
        // Find total folder size for decompression
        size_t folder_size = 0;
        for (const auto& f : pimpl_->files_) {
            if (f.folder_index == entry.folder_index) {
                folder_size = std::max(folder_size,
                                       f.folder_offset + f.uncompressed_size);
            }
        }

        auto folder_data = decompress_folder(entry.folder_index, folder_size);
        if (!folder_data) return std::unexpected(folder_data.error());

        if (entry.folder_offset + entry.uncompressed_size > folder_data->size()) {
            return std::unexpected(error{
                error_code::CorruptData,
                "File extends beyond folder data"
            });
        }

        byte_vector result(entry.uncompressed_size);
        std::copy_n(folder_data->begin() + static_cast <std::ptrdiff_t>(entry.folder_offset),
                    entry.uncompressed_size, result.begin());

        if (byte_progress_cb_) {
            byte_progress_cb_(entry, result.size(), result.size());
        }

        return result;
    }

    void_result_t cab_archive::parse() {
        auto result = parse_header();
        if (!result) return std::unexpected(result.error());

        result = parse_folders();
        if (!result) return std::unexpected(result.error());

        result = parse_files();
        if (!result) return std::unexpected(result.error());

        return {};
    }

    void_result_t cab_archive::parse_header() {
        // Check signature first (only need 4 bytes)
        if (pimpl_->data_.size() < 4 || std::memcmp(pimpl_->data_.data(), cab::SIGNATURE, 4) != 0) {
            return std::unexpected(error{error_code::InvalidSignature, "Not a valid CAB file"});
        }

        if (pimpl_->data_.size() < cab::HEADER_SIZE) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        const u8* p = pimpl_->data_.data() + 8;
        pimpl_->header_.cabinet_size = read_u32_le(p);
        p += 4;
        p += 4; // reserved
        pimpl_->header_.files_offset = read_u32_le(p);
        p += 4;
        p += 4; // reserved
        pimpl_->header_.version_minor = *p++;
        pimpl_->header_.version_major = *p++;
        pimpl_->header_.num_folders = read_u16_le(p);
        p += 2;
        pimpl_->header_.num_files = read_u16_le(p);
        p += 2;
        pimpl_->header_.flags = read_u16_le(p);
        p += 2;
        pimpl_->header_.set_id = read_u16_le(p);
        p += 2;
        pimpl_->header_.cabinet_index = read_u16_le(p);
        p += 2;

        if (pimpl_->header_.flags & cab::RESERVE_PRESENT) {
            if (p + 4 > pimpl_->data_.data() + pimpl_->data_.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }
            pimpl_->header_.header_reserve_size = read_u16_le(p);
            p += 2;
            pimpl_->header_.folder_reserve_size = *p++;
            pimpl_->header_.data_reserve_size = *p++;
        }

        return {};
    }

    void_result_t cab_archive::parse_folders() {
        size_t offset = cab::HEADER_SIZE;

        if (pimpl_->header_.flags & cab::RESERVE_PRESENT) {
            offset += 4 + static_cast <size_t>(pimpl_->header_.header_reserve_size);
        }

        // Skip previous/next cabinet names if present
        if (pimpl_->header_.flags & cab::PREV_CABINET) {
            // Skip cabinet name (null-terminated string)
            while (offset < pimpl_->data_.size() && pimpl_->data_[offset])
                offset++;
            if (offset >= pimpl_->data_.size()) {
                return std::unexpected(error{error_code::TruncatedArchive, "Truncated previous cabinet name"});
            }
            offset++; // null terminator
            // Skip disk name (null-terminated string)
            while (offset < pimpl_->data_.size() && pimpl_->data_[offset])
                offset++;
            if (offset >= pimpl_->data_.size()) {
                return std::unexpected(error{error_code::TruncatedArchive, "Truncated previous disk name"});
            }
            offset++;
        }
        if (pimpl_->header_.flags & cab::NEXT_CABINET) {
            // Skip cabinet name (null-terminated string)
            while (offset < pimpl_->data_.size() && pimpl_->data_[offset])
                offset++;
            if (offset >= pimpl_->data_.size()) {
                return std::unexpected(error{error_code::TruncatedArchive, "Truncated next cabinet name"});
            }
            offset++;
            // Skip disk name (null-terminated string)
            while (offset < pimpl_->data_.size() && pimpl_->data_[offset])
                offset++;
            if (offset >= pimpl_->data_.size()) {
                return std::unexpected(error{error_code::TruncatedArchive, "Truncated next disk name"});
            }
            offset++;
        }

        pimpl_->folders_.reserve(pimpl_->header_.num_folders);
        for (u16 i = 0; i < pimpl_->header_.num_folders; i++) {
            if (offset + cab::FOLDER_SIZE + pimpl_->header_.folder_reserve_size > pimpl_->data_.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }

            const u8* p = pimpl_->data_.data() + offset;
            cab::folder folder;
            folder.data_offset = read_u32_le(p);
            p += 4;
            folder.num_blocks = read_u16_le(p);
            p += 2;
            folder.comp_type = read_u16_le(p);

            pimpl_->folders_.push_back(folder);
            offset += cab::FOLDER_SIZE + pimpl_->header_.folder_reserve_size;
        }

        return {};
    }

    void_result_t cab_archive::parse_files() {
        size_t offset = pimpl_->header_.files_offset;

        pimpl_->files_.reserve(pimpl_->header_.num_files);
        for (u16 i = 0; i < pimpl_->header_.num_files; i++) {
            if (offset + cab::FILE_HEADER_SIZE > pimpl_->data_.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }

            const u8* p = pimpl_->data_.data() + offset;
            file_entry entry;
            entry.uncompressed_size = read_u32_le(p);
            p += 4;
            entry.folder_offset = read_u32_le(p);
            p += 4;
            entry.folder_index = read_u16_le(p);
            p += 2;
            entry.datetime.date = read_u16_le(p);
            p += 2;
            entry.datetime.time = read_u16_le(p);
            p += 2;
            u16 attribs = read_u16_le(p);
            // p += 2;
            entry.attribs.readonly = attribs & 0x01;
            entry.attribs.hidden = attribs & 0x02;
            entry.attribs.system = attribs & 0x04;
            entry.attribs.archive = attribs & 0x20;
            entry.attribs.name_is_utf8 = attribs & 0x80;

            offset += cab::FILE_HEADER_SIZE;

            // Read filename (null-terminated string)
            size_t name_start = offset;
            while (offset < pimpl_->data_.size() && pimpl_->data_[offset])
                offset++;
            if (offset >= pimpl_->data_.size()) {
                return std::unexpected(error{error_code::TruncatedArchive, "Truncated filename in file entry"});
            }
            std::string raw_name(reinterpret_cast <const char*>(pimpl_->data_.data() + name_start), offset - name_start);
            // Sanitize filename to prevent directory traversal attacks
            entry.name = sanitize_path(raw_name);
            offset++; // null terminator

            pimpl_->files_.push_back(std::move(entry));
        }

        return {};
    }

    result_t <byte_vector> cab_archive::decompress_folder(u32 folder_index, size_t max_size) {
        if (folder_index >= pimpl_->folders_.size()) {
            return std::unexpected(error{error_code::FolderNotFound});
        }

        // Check cache
        if (auto cached = pimpl_->folder_cache.get(folder_index)) {
            return **cached;
        }

        const auto& folder = pimpl_->folders_[folder_index];
        auto comp = static_cast <CompressionType>(folder.comp_type & 0x000F);

        // Create appropriate decompressor
        std::unique_ptr <decompressor> decompressor;
        switch (comp) {
            case CompressionType::None:
                break;
            case CompressionType::MSZIP:
                decompressor = std::make_unique <mszip_decompressor>();
                break;
            case CompressionType::LZX: {
                unsigned window_bits = lzx_window_bits(folder.comp_type);
                if (window_bits < 15 || window_bits > 21)
                    window_bits = 21;
                decompressor = std::make_unique <lzx_decompressor>(window_bits);
                break;
            }
            case CompressionType::Quantum: {
                unsigned window_bits = (folder.comp_type >> 8) & 0x1F;
                if (window_bits < 10 || window_bits > 21)
                    window_bits = 21;
                decompressor = std::make_unique <quantum_decompressor>(window_bits);
                break;
            }
            default:
                return std::unexpected(error{error_code::UnsupportedCompression});
        }

        byte_vector output;
        output.reserve(max_size);

        size_t data_offset = folder.data_offset;

        for (u16 block = 0; block < folder.num_blocks; block++) {
            if (data_offset + cab::DATA_HEADER_SIZE + pimpl_->header_.data_reserve_size > pimpl_->data_.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }

            const u8* p = pimpl_->data_.data() + data_offset;
            cab::data_block block_hdr;
            block_hdr.checksum = read_u32_le(p);
            p += 4;
            block_hdr.compressed_size = read_u16_le(p);
            p += 2;
            block_hdr.uncompressed_size = read_u16_le(p);

            data_offset += cab::DATA_HEADER_SIZE + pimpl_->header_.data_reserve_size;

            if (data_offset + block_hdr.compressed_size > pimpl_->data_.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }

            byte_span compressed(pimpl_->data_.data() + data_offset, block_hdr.compressed_size);

            if (comp == CompressionType::None) {
                output.insert(output.end(), compressed.begin(), compressed.end());
            } else {
                byte_vector block_output(block_hdr.uncompressed_size);
                auto result = decompressor->decompress(compressed, block_output);
                if (!result)
                    return std::unexpected(result.error());
                output.insert(output.end(), block_output.begin(),
                              block_output.begin() + static_cast <std::ptrdiff_t>(*result));
            }

            data_offset += block_hdr.compressed_size;

            if (output.size() >= max_size)
                break;
        }

        pimpl_->folder_cache.put(folder_index, output);
        return output;
    }

    cab_archive::~cab_archive() = default;
} // namespace crate
