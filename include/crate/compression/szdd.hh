#pragma once

#include <crate/core/decompressor.hh>

namespace crate {
    namespace szdd {
        // Standard SZDD signature
        constexpr u8 SIGNATURE[] = {'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33};

        // QBasic 4.5 variant signature
        constexpr u8 QBASIC_SIGNATURE[] = {'S', 'Z', ' ', 0x88, 0xF0, 0x27, 0x33};

        struct CRATE_EXPORT header {
            u8 comp_method = 0;
            char missing_char = 0; // Last character of original filename
            u32 uncompressed_size = 0;
            bool is_qbasic = false;
        };
    }

    /// SZDD decompressor (MS-DOS COMPRESS.EXE format)
    /// Decompresses files compressed with MS-DOS COMPRESS.EXE utility.
    /// Uses LZSS compression with a 4096-byte window.
    /// Also supports the QBasic 4.5 variant.
    class CRATE_EXPORT szdd_decompressor : public decompressor {
        public:
            szdd_decompressor();
            ~szdd_decompressor() override;

            result_t<size_t> decompress(byte_span input, mutable_byte_span output) override;
            void reset() override;

            // Parse SZDD header from data
            static result_t<szdd::header> parse_header(byte_span data);

            // Recover original filename from compressed filename
            static std::string recover_filename(std::string_view compressed_name, char missing_char);

        private:
            struct impl;
            std::unique_ptr<impl> pimpl_;
    };
} // namespace crate
