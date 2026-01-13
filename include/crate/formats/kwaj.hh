#pragma once

#include <crate/formats/archive.hh>

namespace crate {
    namespace kwaj {
        constexpr u8 SIGNATURE1[] = {'K', 'W', 'A', 'J'};
        // Second signature has two variants: 0x33 (original) and 0xD1 (MS variant)
        constexpr u8 SIGNATURE2_PART[] = {0x88, 0xF0, 0x27};

        enum flags : u16 {
            HAS_UNCOMPRESSED_LEN = 0x0001,
            HAS_UNKNOWN = 0x0002,
            HAS_DECOMPRESSED_LEN = 0x0004,
            HAS_FILENAME = 0x0008,
            HAS_EXTENSION = 0x0010
        };

        enum method : u16 {
            NONE = 0,
            XOR_FF = 1, // XOR with 0xFF
            SZDD = 2,
            LZH = 3,
            MSZIP = 4
        };

        struct CRATE_EXPORT header {
            u16 comp_method = 0;
            u16 data_offset = 0;
            u16 flags = 0;
            u32 uncompressed_len = 0;
            u32 unknown = 0;
            u32 decompressed_len = 0;
            std::string filename;
            std::string extension;
        };
    }

    class CRATE_EXPORT kwaj_extractor {
        public:
            static result_t <kwaj::header> parse_header(byte_span data);

            static result_t <byte_vector> extract(byte_span data);

            static result_t <byte_vector> extract(const std::filesystem::path& path);

        private:
            static result_t <byte_vector> decompress_xor(byte_span data);

            static result_t <byte_vector> decompress_szdd(byte_span data, u32 expected_size);

            static result_t <byte_vector> decompress_lzh(byte_span data, u32 expected_size);

            static result_t <byte_vector> decompress_mszip(byte_span data, u32 expected_size);
    };
} // namespace crate
