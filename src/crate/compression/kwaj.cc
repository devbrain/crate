#include <crate/compression/kwaj.hh>
#include "crate/compression/lzss.hh"
#include "crate/compression/mszip.hh"
#include <cstring>

namespace crate {

    struct kwaj_decompressor::impl {
        szdd_lzss_decompressor szdd_lzss;
        kwaj_lzss_decompressor kwaj_lzss;
        mszip_decompressor mszip;
    };

    kwaj_decompressor::kwaj_decompressor() : pimpl_(std::make_unique<impl>()) {}

    kwaj_decompressor::~kwaj_decompressor() = default;

    result_t<kwaj::header> kwaj_decompressor::parse_header(byte_span data) {
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
            header.filename = std::string(reinterpret_cast<const char*>(data.data() + name_start), name_len);
            if (pos < data.size() && data[pos] == 0) {
                pos++; // Skip null terminator
            }
        }

        if (header.flags & kwaj::HAS_EXTENSION) {
            // Extension length calculation differs based on whether filename is present
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
                header.extension = std::string(reinterpret_cast<const char*>(data.data() + pos), ext_len);
                pos += ext_len;
            }
        }

        return header;
    }

    result_t<size_t> kwaj_decompressor::decompress(byte_span input, mutable_byte_span output) {
        auto header_result = parse_header(input);
        if (!header_result) return std::unexpected(header_result.error());

        const auto& header = *header_result;

        if (header.data_offset >= input.size()) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        byte_span compressed = input.subspan(header.data_offset);
        size_t result_size = 0;

        switch (static_cast<kwaj::method>(header.comp_method)) {
            case kwaj::NONE:
                if (output.size() < compressed.size()) {
                    return std::unexpected(error{error_code::OutputBufferOverflow});
                }
                std::memcpy(output.data(), compressed.data(), compressed.size());
                result_size = compressed.size();
                break;

            case kwaj::XOR_FF: {
                auto result = decompress_xor(compressed, output);
                if (!result) return std::unexpected(result.error());
                result_size = *result;
                break;
            }

            case kwaj::SZDD: {
                auto result = decompress_szdd(compressed, output, header.decompressed_len);
                if (!result) return std::unexpected(result.error());
                result_size = *result;
                break;
            }

            case kwaj::LZH: {
                auto result = decompress_lzh(compressed, output, header.decompressed_len);
                if (!result) return std::unexpected(result.error());
                result_size = *result;
                break;
            }

            case kwaj::MSZIP: {
                auto result = decompress_mszip(compressed, output, header.decompressed_len);
                if (!result) return std::unexpected(result.error());
                result_size = *result;
                break;
            }

            default:
                return std::unexpected(error{
                    error_code::UnsupportedCompression,
                    "Unknown KWAJ compression method"
                });
        }

        report_progress(result_size, header.decompressed_len);
        return result_size;
    }

    void kwaj_decompressor::reset() {
        pimpl_->szdd_lzss.reset();
        pimpl_->kwaj_lzss.reset();
        pimpl_->mszip.reset();
    }

    result_t<size_t> kwaj_decompressor::decompress_xor(byte_span data, mutable_byte_span output) {
        if (output.size() < data.size()) {
            return std::unexpected(error{error_code::OutputBufferOverflow});
        }
        for (size_t i = 0; i < data.size(); i++) {
            output[i] = static_cast<byte>(data[i] ^ 0xFF);
        }
        return data.size();
    }

    result_t<size_t> kwaj_decompressor::decompress_szdd(byte_span data, mutable_byte_span output, u32 /*expected_size*/) {
        return pimpl_->szdd_lzss.decompress(data, output);
    }

    result_t<size_t> kwaj_decompressor::decompress_lzh(byte_span data, mutable_byte_span output, u32 /*expected_size*/) {
        return pimpl_->kwaj_lzss.decompress(data, output);
    }

    result_t<size_t> kwaj_decompressor::decompress_mszip(byte_span data, mutable_byte_span output, u32 /*expected_size*/) {
        return pimpl_->mszip.decompress(data, output);
    }

} // namespace crate
