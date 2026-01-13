#include <crate/formats/kwaj.hh>
#include <crate/compression/lzss.hh>
#include <crate/compression/mszip.hh>
#include <cstring>

namespace crate {
    result_t <kwaj::header> kwaj_extractor::parse_header(byte_span data) {
        if (data.size() < 14) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        // Check signatures - KWAJ signature + variant marker (0x33 or 0xD1)
        if (std::memcmp(data.data(), kwaj::SIGNATURE1, 4) != 0 ||
            std::memcmp(data.data() + 4, kwaj::SIGNATURE2_PART, 3) != 0) {
            return std::unexpected(error{
                error_code::InvalidSignature,
                "Not a valid KWAJ file"
            });
        }
        // Byte 7 can be 0x33 or 0xD1 (two known variants)
        u8 variant = data[7];
        if (variant != 0x33 && variant != 0xD1) {
            return std::unexpected(error{
                error_code::InvalidSignature,
                "Unknown KWAJ variant"
            });
        }

        kwaj::header header;
        const u8* p = data.data() + 8;
        header.comp_method = read_u16_le(p);
        p += 2;
        header.data_offset = read_u16_le(p);
        p += 2;
        header.flags = read_u16_le(p);
        p += 2;

        size_t pos = 14;

        if (header.flags & kwaj::HAS_UNCOMPRESSED_LEN) {
            if (pos + 4 > data.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }
            header.uncompressed_len = read_u32_le(data.data() + pos);
            pos += 4;
        }

        if (header.flags & kwaj::HAS_UNKNOWN) {
            if (pos + 4 > data.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }
            header.unknown = read_u32_le(data.data() + pos);
            pos += 4;
        }

        if (header.flags & kwaj::HAS_DECOMPRESSED_LEN) {
            if (pos + 4 > data.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }
            header.decompressed_len = read_u32_le(data.data() + pos);
            pos += 4;
        }

        if (header.flags & kwaj::HAS_FILENAME) {
            // Filename is null-terminated, max 8 characters
            if (pos >= data.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }
            size_t name_start = pos;
            while (pos < data.size() && data[pos] != 0) {
                pos++;
            }
            size_t name_len = pos - name_start;
            if (name_len > 8) {
                return std::unexpected(error{
                    error_code::InvalidHeader,
                    "KWAJ filename exceeds 8 characters"
                });
            }
            header.filename = std::string(reinterpret_cast <const char*>(data.data() + name_start), name_len);
            if (pos < data.size() && data[pos] == 0) {
                pos++; // Skip null terminator
            }
        }

        if (header.flags & kwaj::HAS_EXTENSION) {
            // Extension length calculation differs based on whether filename is present:
            // - Extension only: data_offset points AFTER extension (ext_len = data_offset - pos)
            // - With filename: data_offset points to LAST byte of extension (ext_len = data_offset - pos + 1)
            size_t ext_len = 0;
            if (header.flags & kwaj::HAS_FILENAME) {
                // With filename: data_offset is inclusive end of extension
                if (header.data_offset >= pos) {
                    ext_len = header.data_offset - pos + 1;
                }
            } else {
                // Extension only: data_offset is start of compressed data
                if (header.data_offset > pos) {
                    ext_len = header.data_offset - pos;
                }
            }

            if (ext_len > 0 && pos + ext_len <= data.size()) {
                if (ext_len > 3) {
                    return std::unexpected(error{
                        error_code::InvalidHeader,
                        "KWAJ extension exceeds 3 characters"
                    });
                }
                header.extension = std::string(reinterpret_cast <const char*>(data.data() + pos), ext_len);
                pos += ext_len;
            }
        }

        return header;
    }

    result_t <byte_vector> kwaj_extractor::extract(byte_span data) {
        auto header_result = parse_header(data);
        if (!header_result) return std::unexpected(header_result.error());

        const auto& header = *header_result;

        if (header.data_offset >= data.size()) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        byte_span compressed = data.subspan(header.data_offset);

        switch (static_cast <kwaj::method>(header.comp_method)) {
            case kwaj::NONE:
                return byte_vector(compressed.begin(), compressed.end());

            case kwaj::XOR_FF:
                return decompress_xor(compressed);

            case kwaj::SZDD:
                return decompress_szdd(compressed, header.decompressed_len);

            case kwaj::LZH:
                return decompress_lzh(compressed, header.decompressed_len);

            case kwaj::MSZIP:
                return decompress_mszip(compressed, header.decompressed_len);

            default:
                return std::unexpected(error{
                    error_code::UnsupportedCompression,
                    "Unknown KWAJ compression method"
                });
        }
    }

    result_t <byte_vector> kwaj_extractor::extract(const std::filesystem::path& path) {
        auto file = file_input_stream::open(path);
        if (!file) return std::unexpected(file.error());

        auto size = file->size();
        if (!size) return std::unexpected(size.error());

        byte_vector data(*size);
        auto read = file->read(data);
        if (!read) return std::unexpected(read.error());

        return extract(data);
    }

    result_t <byte_vector> kwaj_extractor::decompress_xor(byte_span data) {
        byte_vector result(data.size());
        for (size_t i = 0; i < data.size(); i++) {
            result[i] = data[i] ^ 0xFF;
        }
        return result;
    }

    result_t <byte_vector> kwaj_extractor::decompress_szdd(byte_span data, u32 expected_size) {
        szdd_lzss_decompressor decompressor;
        size_t output_size = expected_size > 0 ? expected_size : 64 * 1024;
        byte_vector output(output_size);

        auto result = decompressor.decompress(data, output);
        if (!result) return std::unexpected(result.error());

        output.resize(*result);
        return output;
    }

    result_t <byte_vector> kwaj_extractor::decompress_lzh(byte_span data, u32 expected_size) {
        kwaj_lzss_decompressor decompressor;
        size_t output_size = expected_size > 0 ? expected_size : 64 * 1024;
        byte_vector output(output_size);

        auto result = decompressor.decompress(data, output);
        if (!result) return std::unexpected(result.error());

        output.resize(*result);
        return output;
    }

    result_t <byte_vector> kwaj_extractor::decompress_mszip(byte_span data, u32 expected_size) {
        mszip_decompressor decompressor;
        size_t output_size = expected_size > 0 ? expected_size : 64 * 1024;
        byte_vector output(output_size);

        auto result = decompressor.decompress(data, output);
        if (!result) return std::unexpected(result.error());

        output.resize(*result);
        return output;
    }
} // namespace crate
