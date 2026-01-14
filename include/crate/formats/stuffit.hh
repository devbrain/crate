#pragma once

#include <crate/formats/archive.hh>

namespace crate {
    namespace stuffit {
        // StuffIt compression methods
        enum class compression_method : u8 {
            none = 0, // Uncompressed
            rle = 1, // RLE90
            lzw = 2, // LZW (Unix compress style)
            huffman = 3, // Huffman
            lzah = 5, // LZAH (LHA -lh1-)
            fixed_huffman = 6, // Fixed Huffman + PackBits
            mw = 8, // MW (not supported)
            lz_huffman = 13, // LZ+Huffman (not supported)
            installer = 14, // Installer (not supported)
            arsenic = 15, // Arsenic (not supported)
        };

        // StuffIt format version
        enum class format_version {
            old_format, // SIT! signature
            v5_format, // StuffIt signature (v5+)
        };
    } // namespace stuffit

    /// StuffIt archive (.sit)
    /// Supports both old format (SIT!) and v5 format (StuffIt).
    /// Common on classic Macintosh systems.
    class CRATE_EXPORT stuffit_archive : public archive {
        public:
            static result_t <std::unique_ptr <stuffit_archive>> open(byte_span data);

            ~stuffit_archive() override;

            [[nodiscard]] const std::vector <file_entry>& files() const override;

            using archive::extract;
            [[nodiscard]] result_t <byte_vector> extract(const file_entry& entry) override;

            /// Get the archive format version
            [[nodiscard]] stuffit::format_version format() const;

        private:
            stuffit_archive();
            struct impl;
            std::unique_ptr <impl> pimpl_;
    };
} // namespace crate
