#include <crate/formats/libarchive_archive.hh>
#include <crate/core/path.hh>
#include <archive.h>
#include <archive_entry.h>
#include <fstream>
#include <cstring>

// Use explicit struct keyword to avoid conflict with crate::archive class
using la_archive = struct archive;
using la_archive_entry = struct archive_entry;

namespace crate {

namespace {
    // Custom read callback data
    struct memory_read_data {
        const byte* data;
        size_t size;
        size_t pos;
    };

    la_ssize_t memory_read_callback(la_archive*, void* client_data, const void** buffer) {
        auto* rd = static_cast<memory_read_data*>(client_data);
        if (rd->pos >= rd->size) {
            return 0;
        }
        *buffer = rd->data + rd->pos;
        size_t remaining = rd->size - rd->pos;
        size_t to_read = remaining > 65536 ? 65536 : remaining;
        rd->pos += to_read;
        return static_cast<la_ssize_t>(to_read);
    }

    int memory_close_callback(la_archive*, void*) {
        return ARCHIVE_OK;
    }

    // Convert libarchive filetype to our is_directory flag
    bool is_directory_entry(la_archive_entry* entry) {
        // AE_IFDIR uses old-style cast, so we compare directly with the value
        constexpr unsigned int dir_mode = 0040000;
        return archive_entry_filetype(entry) == dir_mode;
    }

    // Convert Unix timestamp to DOS datetime
    dos_date_time unix_to_dos_datetime(time_t t) {
        dos_date_time dt{};
        if (t <= 0) {
            // Default to 1980-01-01 00:00:00
            dt.date = (1 << 5) | 1;  // month=1, day=1, year=0 (1980)
            dt.time = 0;
            return dt;
        }

        struct tm* tm_val = localtime(&t);
        if (!tm_val) {
            dt.date = (1 << 5) | 1;
            dt.time = 0;
            return dt;
        }

        int year = tm_val->tm_year + 1900;
        if (year < 1980) year = 1980;
        if (year > 2107) year = 2107;

        dt.date = static_cast<u16>(
            ((year - 1980) << 9) |
            ((tm_val->tm_mon + 1) << 5) |
            tm_val->tm_mday
        );
        dt.time = static_cast<u16>(
            (tm_val->tm_hour << 11) |
            (tm_val->tm_min << 5) |
            (tm_val->tm_sec / 2)
        );
        return dt;
    }
}

struct libarchive_archive::impl {
    byte_vector data_;
    std::vector<file_entry> files_;
    std::string format_name_;

    // Store entry metadata for extraction
    struct entry_info {
        std::string pathname;
        i64 size;
        i64 offset;  // Offset in original data where entry starts (for re-reading)
    };
    std::vector<entry_info> entries_;
};

libarchive_archive::libarchive_archive()
    : pimpl_(std::make_unique<impl>()) {
}

libarchive_archive::~libarchive_archive() = default;

result_t<std::unique_ptr<libarchive_archive>> libarchive_archive::open(byte_span data) {
    auto archive = std::unique_ptr<libarchive_archive>(new libarchive_archive());
    archive->pimpl_->data_.assign(data.begin(), data.end());

    auto result = archive->parse();
    if (!result) {
        return std::unexpected(result.error());
    }

    return archive;
}

result_t<std::unique_ptr<libarchive_archive>> libarchive_archive::open(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(error{error_code::FileNotFound, "Cannot open file"});
    }

    file.seekg(0, std::ios::end);
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    byte_vector data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);

    return open(data);
}

const std::vector<file_entry>& libarchive_archive::files() const {
    return pimpl_->files_;
}

std::string libarchive_archive::format_name() const {
    return pimpl_->format_name_;
}

void_result_t libarchive_archive::parse() {
    la_archive* a = archive_read_new();
    if (!a) {
        return std::unexpected(error{error_code::CorruptData, "Failed to create archive reader"});
    }

    // Enable all formats (core libarchive supports these without compression libs)
    archive_read_support_format_all(a);
    // Enable raw format as fallback
    archive_read_support_format_raw(a);

    // Set up memory reading
    memory_read_data rd{pimpl_->data_.data(), pimpl_->data_.size(), 0};

    int r = archive_read_open(a, &rd, nullptr, memory_read_callback, memory_close_callback);
    if (r != ARCHIVE_OK) {
        std::string err = archive_error_string(a) ? archive_error_string(a) : "Unknown error";
        archive_read_free(a);
        return std::unexpected(error{error_code::InvalidSignature, "Failed to open archive: " + err});
    }

    // Get format name
    // Note: format name is available after reading first entry
    la_archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        if (pimpl_->format_name_.empty()) {
            const char* fmt = archive_format_name(a);
            if (fmt) {
                pimpl_->format_name_ = fmt;
            }
        }

        file_entry fe;
        const char* pathname = archive_entry_pathname(entry);
        fe.name = pathname ? sanitize_path(pathname) : "";
        fe.uncompressed_size = static_cast<u64>(archive_entry_size(entry));
        fe.compressed_size = fe.uncompressed_size;  // libarchive doesn't expose compressed size easily
        fe.is_directory = is_directory_entry(entry);
        fe.is_encrypted = archive_entry_is_encrypted(entry) != 0;
        fe.datetime = unix_to_dos_datetime(archive_entry_mtime(entry));

        // Store entry index for extraction
        fe.folder_index = static_cast<u32>(pimpl_->entries_.size());

        impl::entry_info ei;
        ei.pathname = pathname ? pathname : "";
        ei.size = archive_entry_size(entry);
        ei.offset = 0;  // Will need to re-scan for extraction

        pimpl_->entries_.push_back(std::move(ei));
        pimpl_->files_.push_back(std::move(fe));

        // Skip the data for now (we'll read it during extraction)
        archive_read_data_skip(a);
    }

    archive_read_free(a);
    return {};
}

result_t<byte_vector> libarchive_archive::extract(const file_entry& entry) {
    if (entry.folder_index >= pimpl_->entries_.size()) {
        return std::unexpected(error{error_code::InvalidParameter, "Invalid entry index"});
    }

    // Re-open archive and seek to the entry
    la_archive* a = archive_read_new();
    if (!a) {
        return std::unexpected(error{error_code::CorruptData, "Failed to create archive reader"});
    }

    archive_read_support_format_all(a);
    archive_read_support_format_raw(a);

    memory_read_data rd{pimpl_->data_.data(), pimpl_->data_.size(), 0};

    int r = archive_read_open(a, &rd, nullptr, memory_read_callback, memory_close_callback);
    if (r != ARCHIVE_OK) {
        archive_read_free(a);
        return std::unexpected(error{error_code::CorruptData, "Failed to reopen archive"});
    }

    // Seek to the correct entry
    la_archive_entry* ae;
    u32 idx = 0;
    while (archive_read_next_header(a, &ae) == ARCHIVE_OK) {
        if (idx == entry.folder_index) {
            // Found the entry, read its data
            i64 size = archive_entry_size(ae);
            if (size < 0) {
                // Unknown size, read in chunks
                byte_vector output;
                byte buffer[65536];
                la_ssize_t bytes_read;
                while ((bytes_read = archive_read_data(a, buffer, sizeof(buffer))) > 0) {
                    output.insert(output.end(), buffer, buffer + bytes_read);
                }
                if (bytes_read < 0) {
                    archive_read_free(a);
                    return std::unexpected(error{error_code::CorruptData, "Failed to read entry data"});
                }
                archive_read_free(a);
                if (byte_progress_cb_) {
                    byte_progress_cb_(entry, output.size(), output.size());
                }
                return output;
            } else {
                byte_vector output(static_cast<size_t>(size));
                la_ssize_t bytes_read = archive_read_data(a, output.data(), output.size());
                archive_read_free(a);

                if (bytes_read < 0) {
                    return std::unexpected(error{error_code::CorruptData, "Failed to read entry data"});
                }
                if (static_cast<size_t>(bytes_read) != output.size()) {
                    output.resize(static_cast<size_t>(bytes_read));
                }
                if (byte_progress_cb_) {
                    byte_progress_cb_(entry, output.size(), output.size());
                }
                return output;
            }
        }
        archive_read_data_skip(a);
        ++idx;
    }

    archive_read_free(a);
    return std::unexpected(error{error_code::FileNotFound, "Entry not found in archive"});
}

} // namespace crate
