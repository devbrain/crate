#pragma once

#include <crate/core/types.hh>
#include <crate/core/error.hh>
#include <crate/core/system.hh>
#include <functional>
#include <filesystem>
#include <concepts>

namespace crate {
    // File entry information
    struct CRATE_EXPORT file_entry {
        std::string name;
        u64 uncompressed_size = 0;
        u64 compressed_size = 0;
        dos_date_time datetime{};
        file_attributes attribs{};
        u32 folder_index = 0;
        u64 folder_offset = 0;
        bool is_directory = false;
        bool is_encrypted = false;

        /// Check if this entry represents a directory
        [[nodiscard]] bool directory() const;

        /// Check if this entry is encrypted
        [[nodiscard]] bool encrypted() const;

        /// Get compression ratio (0.0 = no compression, 1.0 = fully compressed)
        /// Returns 0.0 for empty files or directories
        [[nodiscard]] double compression_ratio() const;

        /// Get compression percentage (100 = fully compressed, 0 = no compression)
        [[nodiscard]] int compression_percent() const;
    };

    // Archive interface
    class CRATE_EXPORT archive {
        public:
            virtual ~archive() = default;

            // Get list of files
            [[nodiscard]] virtual const std::vector <file_entry>& files() const = 0;

            // Extract a file to memory (must be implemented by derived classes)
            [[nodiscard]] virtual result_t <byte_vector> extract(const file_entry& entry) = 0;

            // Extract a file to an output stream (default implementation uses extract to memory)
            [[nodiscard]] virtual result_t <size_t> extract_to(const file_entry& entry, output_stream& dest);

            // Extract a file to disk (default implementation using extract to memory)
            virtual void_result_t extract(const file_entry& entry,
                                          const std::filesystem::path& dest);

            // Extract all files (default implementation)
            virtual void_result_t extract_all(const std::filesystem::path& dest_dir);

            // Callback for file-level progress reporting (file N of M)
            using progress_callback_t = std::function<void(const file_entry&, size_t current, size_t total)>;
            virtual void set_progress_callback(progress_callback_t cb);

            // Callback for byte-level progress within a file (bytes_written, total_expected)
            // total_expected is the uncompressed size, may be 0 if unknown
            using byte_progress_callback_t = std::function<void(const file_entry&, size_t bytes_written, size_t total_expected)>;
            virtual void set_byte_progress_callback(byte_progress_callback_t cb);

        protected:
            progress_callback_t progress_cb_;
            byte_progress_callback_t byte_progress_cb_;
    };

    /// Concept for types that can be used as archives
    /// Requires:
    /// - Static open(ByteSpan) method returning Result<unique_ptr<T>>
    /// - files() method returning const vector<FileEntry>&
    /// - extract(FileEntry) method returning Result<ByteVector>
    template<typename T>
    concept ArchiveLike = requires(T& archive, const T& const_archive, byte_span data, const file_entry& entry)
    {
        // Must have a static open method
        { T::open(data) } -> std::same_as <result_t <std::unique_ptr <T>>>;
        // Must provide file listing
        { const_archive.files() } -> std::same_as <const std::vector <file_entry>&>;
        // Must be able to extract files
        { archive.extract(entry) } -> std::same_as <result_t <byte_vector>>;
    };
} // namespace crate
