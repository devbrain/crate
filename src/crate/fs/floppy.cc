#include <crate/formats/floppy.hh>
#include <crate/core/system.hh>
#include <crate/fs/fat_bpb.hh>
#include <crate/fs/fat_table.hh>
#include <crate/fs/fat_directory.hh>

namespace crate {

struct floppy_image::impl {
    byte_vector data;
    fs::bpb bpb;
    std::unique_ptr<fs::fat_table> fat;
    std::vector<file_entry> files;

    // Internal entry info for extraction
    struct internal_entry {
        u32 first_cluster;
        u32 size;
    };
    std::vector<internal_entry> internal_entries;

    // Scan directory recursively
    void scan_directory(u32 cluster, const std::string& prefix, int depth);

    // Read cluster data
    byte_span read_cluster(u32 cluster) const;

    // Read directory data (root or subdirectory)
    byte_vector read_directory_data(u32 cluster) const;
};

byte_span floppy_image::impl::read_cluster(u32 cluster) const {
    u32 offset = fs::cluster_offset(bpb, cluster);
    u32 size = bpb.bytes_per_cluster;

    if (offset + size > data.size()) {
        return {};
    }

    return byte_span(data.data() + offset, size);
}

byte_vector floppy_image::impl::read_directory_data(u32 cluster) const {
    if (cluster == 0) {
        // Root directory - fixed location and size
        if (bpb.root_dir_offset + bpb.root_dir_size > data.size()) {
            return {};
        }
        return byte_vector(data.begin() + bpb.root_dir_offset,
                          data.begin() + bpb.root_dir_offset + bpb.root_dir_size);
    }

    // Subdirectory - follow cluster chain
    byte_vector result;
    auto chain = fat->chain(cluster);

    for (u32 c : chain) {
        auto cluster_data = read_cluster(c);
        if (cluster_data.empty()) {
            break;
        }
        result.insert(result.end(), cluster_data.begin(), cluster_data.end());
    }

    return result;
}

void floppy_image::impl::scan_directory(u32 cluster, const std::string& prefix, int depth) {
    // Prevent infinite recursion
    if (depth > 256) {
        return;
    }

    byte_vector dir_data = read_directory_data(cluster);
    if (dir_data.empty()) {
        return;
    }

    u32 max_entries = (cluster == 0) ? bpb.root_entry_count : 0;
    fs::directory_reader reader(dir_data, max_entries);

    while (auto entry = reader.next()) {
        std::string full_path = prefix.empty() ? entry->name : prefix + "/" + entry->name;

        if (entry->is_directory) {
            // Recurse into subdirectory
            if (entry->first_cluster >= 2) {
                scan_directory(entry->first_cluster, full_path, depth + 1);
            }
        } else {
            // Add file entry
            file_entry fe;
            fe.name = full_path;
            fe.uncompressed_size = entry->size;
            fe.compressed_size = entry->size;  // No compression
            fe.datetime = entry->datetime;
            fe.attribs = entry->attribs;
            fe.is_directory = false;
            fe.is_encrypted = false;
            fe.folder_index = static_cast<u32>(internal_entries.size());

            internal_entry ie;
            ie.first_cluster = entry->first_cluster;
            ie.size = entry->size;

            files.push_back(fe);
            internal_entries.push_back(ie);
        }
    }
}

floppy_image::floppy_image()
    : impl_(std::make_unique<impl>()) {}

floppy_image::~floppy_image() = default;

result_t<std::unique_ptr<floppy_image>> floppy_image::open(byte_span data) {
    if (data.size() < 512) {
        return crate::make_unexpected(error{error_code::TruncatedArchive, "Image too small"});
    }

    // Parse BPB
    auto bpb_result = fs::parse_bpb(data);
    if (!bpb_result) {
        return crate::make_unexpected(bpb_result.error());
    }

    auto image = std::unique_ptr<floppy_image>(new floppy_image());
    image->impl_->data.assign(data.begin(), data.end());
    image->impl_->bpb = *bpb_result;

    // Create FAT table reader
    // IMPORTANT: Use impl_->data (our copy), not the input data parameter!
    const auto& bpb = image->impl_->bpb;
    u32 fat_size = bpb.sectors_per_fat * bpb.bytes_per_sector;

    if (bpb.fat_offset + fat_size > image->impl_->data.size()) {
        return crate::make_unexpected(error{error_code::TruncatedArchive, "FAT table truncated"});
    }

    byte_span fat_data(image->impl_->data.data() + bpb.fat_offset, fat_size);
    image->impl_->fat = std::make_unique<fs::fat_table>(fat_data, bpb.type, bpb.cluster_count);

    // Scan all files starting from root directory
    image->impl_->scan_directory(0, "", 0);

    return image;
}

result_t<std::unique_ptr<floppy_image>> floppy_image::open(const std::filesystem::path& path) {
    auto file = file_input_stream::open(path);
    if (!file) {
        return crate::make_unexpected(file.error());
    }

    auto size = file->size();
    if (!size) {
        return crate::make_unexpected(size.error());
    }

    byte_vector data(*size);
    auto read_result = file->read(data);
    if (!read_result) {
        return crate::make_unexpected(read_result.error());
    }

    return open(data);
}

const std::vector<file_entry>& floppy_image::files() const {
    return impl_->files;
}

result_t<byte_vector> floppy_image::extract(const file_entry& entry) {
    if (entry.folder_index >= impl_->internal_entries.size()) {
        return crate::make_unexpected(error{error_code::FileNotInArchive});
    }

    const auto& ie = impl_->internal_entries[entry.folder_index];

    // Empty file
    if (ie.size == 0) {
        if (byte_progress_cb_) {
            byte_progress_cb_(entry, 0, 0);
        }
        return byte_vector{};
    }

    // File with no clusters (shouldn't happen for non-empty files)
    if (ie.first_cluster < 2) {
        return crate::make_unexpected(error{error_code::CorruptData, "File has no cluster chain"});
    }

    // Read cluster chain
    auto chain = impl_->fat->chain(ie.first_cluster);
    if (chain.empty()) {
        return crate::make_unexpected(error{error_code::CorruptData, "Empty cluster chain"});
    }

    byte_vector result;
    result.reserve(ie.size);

    u32 remaining = ie.size;

    for (u32 cluster : chain) {
        auto cluster_data = impl_->read_cluster(cluster);
        if (cluster_data.empty()) {
            return crate::make_unexpected(error{error_code::CorruptData, "Invalid cluster"});
        }

        u32 to_copy = std::min(remaining, static_cast<u32>(cluster_data.size()));
        result.insert(result.end(), cluster_data.begin(), cluster_data.begin() + to_copy);
        remaining -= to_copy;

        // Report progress
        if (byte_progress_cb_) {
            byte_progress_cb_(entry, result.size(), ie.size);
        }

        if (remaining == 0) {
            break;
        }
    }

    if (result.size() != ie.size) {
        return crate::make_unexpected(error{error_code::CorruptData,
            "File size mismatch: expected " + std::to_string(ie.size) +
            ", got " + std::to_string(result.size())});
    }

    return result;
}

} // namespace crate
