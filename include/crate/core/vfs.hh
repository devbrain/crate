#pragma once

#include <crate/core/types.hh>
#include <crate/core/error.hh>
#include <crate/formats/archive.hh>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cstring>
#include <algorithm>

namespace crate::vfs {
    // VFS entry types
    enum class entry_type {
        File,
        Directory,
        Symlink
    };

    // File/directory information (like stat())
    struct CRATE_EXPORT stat_info {
        entry_type type = entry_type::File;
        u64 size = 0; // Uncompressed size
        u64 compressed_size = 0;
        dos_date_time mtime{};
        file_attributes attribs{};
        bool is_encrypted = false;
    };

    // Directory entry for listings
    struct CRATE_EXPORT dir_entry {
        std::string name; // Entry name (not full path)
        entry_type type = entry_type::File;
        u64 size = 0;
    };

    // Forward declaration
    class CRATE_EXPORT archive_vfs;

    // Streaming file reader interface (zlib-style)
    class CRATE_EXPORT file_reader {
        public:
            virtual ~file_reader();

            // Read up to `size` bytes into buffer
            // Returns bytes actually read (0 = EOF)
            virtual result_t <size_t> read(u8* buffer, size_t size) = 0;

            // Convenience: read into vector (appends to existing data)
            result_t <size_t> read(std::vector <u8>& buffer, size_t max_size);

            // Check if at end of file
            [[nodiscard]] virtual bool eof() const = 0;

            // Current position in uncompressed stream
            [[nodiscard]] virtual u64 position() const = 0;

            // Total uncompressed size
            [[nodiscard]] virtual u64 size() const = 0;

            // Bytes remaining
            [[nodiscard]] u64 remaining() const;
    };

    // Simple memory-based file reader (wraps already-extracted data)
    class CRATE_EXPORT memory_file_reader : public file_reader {
        public:
            explicit memory_file_reader(byte_vector data);

            result_t <size_t> read(u8* buffer, size_t size) override;

            [[nodiscard]] bool eof() const override;

            [[nodiscard]] u64 position() const override;

            [[nodiscard]] u64 size() const override;

        private:
            byte_vector data_;
            size_t pos_ = 0;
    };

    // Internal directory tree node
    struct CRATE_EXPORT vfs_node {
        std::string name;
        entry_type type = entry_type::Directory;
        const file_entry* entry = nullptr; // Points to archive entry for files
        std::unordered_map <std::string, std::unique_ptr <vfs_node>> children;
    };

    // Virtual filesystem wrapper for archives
    class CRATE_EXPORT archive_vfs {
        public:
            // Create VFS from any archive
            template<typename ArchiveType>
            static result_t <std::unique_ptr <archive_vfs>> create(std::unique_ptr <ArchiveType> archive);

            // Check if path exists
            [[nodiscard]] bool exists(std::string_view path) const;

            // Check if path is a file
            [[nodiscard]] bool is_file(std::string_view path) const;

            // Check if path is a directory
            [[nodiscard]] bool is_directory(std::string_view path) const;

            // Get entry info (like stat())
            [[nodiscard]] std::optional <stat_info> stat(std::string_view path) const;

            // List directory contents
            [[nodiscard]] result_t <std::vector <dir_entry>> readdir(std::string_view path) const;

            // Open streaming reader for a file
            [[nodiscard]] result_t <std::unique_ptr <file_reader>> open(std::string_view path) const;

            // Convenience: read entire file at once
            [[nodiscard]] result_t <byte_vector> read(std::string_view path) const;

            // Get the root directory listing
            [[nodiscard]] result_t <std::vector <dir_entry>> root() const;

            // Get total number of files
            [[nodiscard]] size_t file_count() const;

            // Get underlying archive (for advanced operations)
            [[nodiscard]] archive* get_archive() const;

        private:
            archive_vfs();

            // Build directory tree from flat file list
            void build_tree();

            // Insert a file path into the tree
            void insert_path(const file_entry& entry) const;

            // Normalize path: convert backslashes, remove leading/trailing slashes
            static std::string normalize_path(std::string_view path);

            // Find node by path
            [[nodiscard]] const vfs_node* find_node(std::string_view path) const;

            std::unique_ptr <archive> archive_;
            std::unique_ptr <vfs_node> root_;
    };

    template<typename ArchiveType>
    result_t<std::unique_ptr<archive_vfs>> archive_vfs::create(std::unique_ptr<ArchiveType> archive) {
        auto vfs = std::unique_ptr <archive_vfs>(new archive_vfs());
        vfs->archive_ = std::move(archive);
        vfs->build_tree();
        return vfs;
    }


} // namespace crate::vfs
