#pragma once

#include <crate/core/decompressor.hh>

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

/// KWAJ decompressor (MS-DOS COMPRESS.EXE variant format)
/// Decompresses files compressed with MS-DOS COMPRESS.EXE (KWAJ variant).
/// Supports multiple compression methods: none, XOR, SZDD/LZSS, LZH, and MSZIP.
///
/// This decompressor requires all input data in a single call to decompress_some()
/// with input_finished=true. Partial streaming is not supported.
class CRATE_EXPORT kwaj_decompressor : public decompressor {
public:
    kwaj_decompressor();
    ~kwaj_decompressor() override;

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    void reset() override;

    // Parse KWAJ header from data
    static result_t<kwaj::header> parse_header(byte_span data);

private:
    result_t<size_t> decompress_xor(byte_span data, mutable_byte_span output);
    result_t<size_t> decompress_szdd(byte_span data, mutable_byte_span output, u32 expected_size);
    result_t<size_t> decompress_lzh(byte_span data, mutable_byte_span output, u32 expected_size);
    result_t<size_t> decompress_mszip(byte_span data, mutable_byte_span output, u32 expected_size);

    struct impl;
    std::unique_ptr<impl> pimpl_;
};

} // namespace crate
