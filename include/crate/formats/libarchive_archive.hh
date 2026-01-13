#pragma once

#include <crate/formats/archive.hh>

namespace crate {
    /// Archive implementation using libarchive for format support.
    /// This provides access to formats like tar, cpio, iso9660, 7zip, etc.
    /// Note: With core-only libarchive (no compression libs), only uncompressed
    /// archives are supported directly. Compressed data can be handled by
    /// combining with crate's decompressors in a future iteration.
    class CRATE_EXPORT libarchive_archive : public archive {
        public:
            using archive::extract;

            ~libarchive_archive() override;

            /// Open an archive from memory
            static result_t <std::unique_ptr <libarchive_archive>> open(byte_span data);

            /// Open an archive from a file path
            static result_t <std::unique_ptr <libarchive_archive>> open(const std::filesystem::path& path);

            const std::vector <file_entry>& files() const override;
            result_t <byte_vector> extract(const file_entry& entry) override;

            /// Get the archive format name (e.g., "POSIX tar", "7-Zip", "ISO9660")
            [[nodiscard]] std::string format_name() const;

        private:
            libarchive_archive();
            void_result_t parse();

            struct impl;
            std::unique_ptr <impl> pimpl_;
    };
} // namespace crate
