#include <crate/formats/szdd.hh>
#include <crate/compression/lzss.hh>
#include <cstring>

namespace crate {

    result_t <szdd::header> szdd_extractor::parse_header(byte_span data) {
        szdd::header header{};

        if (data.size() < 14) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        // Check for standard SZDD signature
        if (std::memcmp(data.data(), szdd::SIGNATURE, 8) == 0) {
            header.is_qbasic = false;
            header.comp_method = data[8];
            header.missing_char = static_cast <char>(data[9]);
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

    result_t <byte_vector> szdd_extractor::extract(byte_span data) {
        auto header_result = parse_header(data);
        if (!header_result) return std::unexpected(header_result.error());

        const auto& header = *header_result;

        // Calculate data offset
        size_t data_offset = header.is_qbasic ? 11 : 14;

        if (data_offset >= data.size()) {
            // No compressed data after header - return empty result
            return byte_vector{};
        }

        byte_span compressed = data.subspan(data_offset);

        // Decompress using LZSS
        szdd_lzss_decompressor decompressor;
        byte_vector output(header.uncompressed_size);

        auto result = decompressor.decompress(compressed, output);
        if (!result) return std::unexpected(result.error());

        if (*result != header.uncompressed_size) {
            output.resize(*result);
        }

        return output;
    }

    result_t <byte_vector> szdd_extractor::extract(const std::filesystem::path& path) {
        auto file = file_input_stream::open(path);
        if (!file) return std::unexpected(file.error());

        auto size = file->size();
        if (!size) return std::unexpected(size.error());

        byte_vector data(*size);
        auto read = file->read(data);
        if (!read) return std::unexpected(read.error());

        return extract(data);
    }

    std::string szdd_extractor::recover_filename(std::string_view compressed_name, char missing_char) {
        std::string result(compressed_name);

        // Find the underscore placeholder and replace with missing char
        if (auto pos = result.find('_'); pos != std::string::npos && missing_char != '\0') {
            result[pos] = missing_char;
        }

        return result;
    }
} // namespace crate
