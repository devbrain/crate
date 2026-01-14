#include <crate/compression/szdd.hh>
#include "crate/compression/lzss.hh"
#include <cstring>

namespace crate {

    struct szdd_decompressor::impl {
        szdd_lzss_decompressor lzss;
    };

    szdd_decompressor::szdd_decompressor() : pimpl_(std::make_unique<impl>()) {}

    szdd_decompressor::~szdd_decompressor() = default;

    result_t<szdd::header> szdd_decompressor::parse_header(byte_span data) {
        szdd::header header{};

        if (data.size() < 14) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        // Check for standard SZDD signature
        if (std::memcmp(data.data(), szdd::SIGNATURE, 8) == 0) {
            header.is_qbasic = false;
            header.comp_method = data[8];
            header.missing_char = static_cast<char>(data[9]);
            header.uncompressed_size = read_u32_le(data.data() + 10);
        }
        // Check for QBasic variant
        else if (data.size() >= 11 && std::memcmp(data.data(), szdd::QBASIC_SIGNATURE, 7) == 0) {
            header.is_qbasic = true;
            header.comp_method = 'A';
            header.missing_char = '\0';
            header.uncompressed_size = read_u32_le(data.data() + 7);
        } else {
            return std::unexpected(error{
                error_code::InvalidSignature,
                "Not a valid SZDD file"
            });
        }

        if (header.comp_method != 'A') {
            return std::unexpected(error{
                error_code::UnsupportedCompression,
                "Only SZDD compression method 'A' is supported"
            });
        }

        return header;
    }

    result_t<size_t> szdd_decompressor::decompress(byte_span input, mutable_byte_span output) {
        auto header_result = parse_header(input);
        if (!header_result) return std::unexpected(header_result.error());

        const auto& header = *header_result;

        // Calculate data offset
        size_t data_offset = header.is_qbasic ? 11 : 14;

        if (data_offset >= input.size()) {
            // No compressed data after header
            report_progress(0, 0);
            return 0;
        }

        byte_span compressed = input.subspan(data_offset);

        // Check output buffer size
        if (output.size() < header.uncompressed_size) {
            return std::unexpected(error{
                error_code::OutputBufferOverflow,
                "Output buffer too small for decompressed data"
            });
        }

        // Decompress using LZSS
        auto result = pimpl_->lzss.decompress(compressed, output);
        if (!result) return std::unexpected(result.error());

        report_progress(*result, header.uncompressed_size);
        return *result;
    }

    void szdd_decompressor::reset() {
        pimpl_->lzss.reset();
    }

    std::string szdd_decompressor::recover_filename(std::string_view compressed_name, char missing_char) {
        std::string result(compressed_name);

        // Find the underscore placeholder and replace with missing char
        if (auto pos = result.find('_'); pos != std::string::npos && missing_char != '\0') {
            result[pos] = missing_char;
        }

        return result;
    }

} // namespace crate
