#pragma once

#include <crate/formats/archive.hh>

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

    class CRATE_EXPORT szdd_extractor {
        public:
            static result_t <szdd::header> parse_header(byte_span data);

            static result_t <byte_vector> extract(byte_span data);

            static result_t <byte_vector> extract(const std::filesystem::path& path);

            // Recover original filename from compressed filename
            static std::string recover_filename(std::string_view compressed_name, char missing_char);
    };
} // namespace crate
