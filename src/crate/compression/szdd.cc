#include <crate/compression/szdd.hh>
#include "crate/compression/lzss.hh"
#include <cstring>

namespace crate {

    struct szdd_decompressor::impl {
        // State machine
        enum class state : u8 {
            READ_HEADER,      // Buffering header bytes
            DECOMPRESS_DATA,  // Streaming LZSS decompression
            DONE              // Finished
        };

        state current_state = state::READ_HEADER;
        szdd_lzss_decompressor lzss;

        // Header buffering
        std::array<u8, 14> header_buf{};
        size_t header_bytes = 0;

        // Parsed header info
        szdd::header header{};
        size_t total_output = 0;

        void reset_state() {
            current_state = state::READ_HEADER;
            header_buf = {};
            header_bytes = 0;
            header = {};
            total_output = 0;
            lzss.reset();
        }
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

    result_t<stream_result> szdd_decompressor::decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished
    ) {
        size_t total_read = 0;
        size_t total_written = 0;

        while (true) {
            switch (pimpl_->current_state) {
                case impl::state::READ_HEADER: {
                    // Buffer header bytes until we have enough
                    constexpr size_t HEADER_SIZE = 14;  // Max header size
                    size_t need = HEADER_SIZE - pimpl_->header_bytes;
                    size_t available = input.size() - total_read;
                    size_t to_copy = std::min(need, available);

                    if (to_copy > 0) {
                        std::memcpy(pimpl_->header_buf.data() + pimpl_->header_bytes,
                                   input.data() + total_read, to_copy);
                        pimpl_->header_bytes += to_copy;
                        total_read += to_copy;
                    }

                    // Try to parse header once we have enough bytes
                    if (pimpl_->header_bytes >= 8) {
                        byte_span header_span{pimpl_->header_buf.data(), pimpl_->header_bytes};

                        // Check if this could be QBasic variant (signature: "SZ " + magic)
                        bool is_qbasic_sig = std::memcmp(header_span.data(), szdd::QBASIC_SIGNATURE, 7) == 0;
                        // Check if this could be standard SZDD (signature: "SZDD" + magic)
                        bool is_standard_sig = std::memcmp(header_span.data(), szdd::SIGNATURE, 8) == 0;

                        if (!is_qbasic_sig && !is_standard_sig) {
                            // Neither signature matches - invalid file
                            return std::unexpected(error{
                                error_code::InvalidSignature,
                                "Not a valid SZDD file"
                            });
                        }

                        // QBasic variant needs 11 bytes total
                        if (is_qbasic_sig && pimpl_->header_bytes >= 11) {
                            pimpl_->header.is_qbasic = true;
                            pimpl_->header.comp_method = 'A';
                            pimpl_->header.missing_char = '\0';
                            pimpl_->header.uncompressed_size = read_u32_le(header_span.data() + 7);
                            pimpl_->current_state = impl::state::DECOMPRESS_DATA;
                            // "Un-read" extra header bytes we buffered
                            size_t extra = pimpl_->header_bytes - 11;
                            total_read -= extra;
                            continue;
                        }

                        // Standard SZDD needs 14 bytes total
                        if (is_standard_sig && pimpl_->header_bytes >= 14) {
                            pimpl_->header.is_qbasic = false;
                            pimpl_->header.comp_method = header_span[8];
                            pimpl_->header.missing_char = static_cast<char>(header_span[9]);
                            pimpl_->header.uncompressed_size = read_u32_le(header_span.data() + 10);

                            if (pimpl_->header.comp_method != 'A') {
                                return std::unexpected(error{
                                    error_code::UnsupportedCompression,
                                    "Only SZDD compression method 'A' is supported"
                                });
                            }

                            pimpl_->current_state = impl::state::DECOMPRESS_DATA;
                            continue;
                        }

                        // Valid signature prefix but need more bytes - fall through
                    }

                    // Need more header bytes - check if truncated
                    if (input_finished) {
                        // We need at least 8 bytes to check signatures
                        if (pimpl_->header_bytes < 8) {
                            return std::unexpected(error{error_code::TruncatedArchive,
                                "Incomplete SZDD header"});
                        }
                        // If we have a valid signature but not enough bytes, it's truncated
                        // (Invalid signature already handled above)
                        return std::unexpected(error{error_code::TruncatedArchive,
                            "Incomplete SZDD header"});
                    }

                    // Return: need more input
                    return stream_result::need_input(total_read, total_written);
                }

                case impl::state::DECOMPRESS_DATA: {
                    // Delegate to streaming LZSS decompressor
                    byte_span remaining_input{input.data() + total_read, input.size() - total_read};
                    mutable_byte_span remaining_output{output.data() + total_written,
                                                       output.size() - total_written};

                    auto result = pimpl_->lzss.decompress_some(
                        remaining_input, remaining_output, input_finished);

                    if (!result) {
                        return std::unexpected(result.error());
                    }

                    total_read += result->bytes_read;
                    total_written += result->bytes_written;
                    pimpl_->total_output += result->bytes_written;

                    if (result->bytes_written > 0) {
                        report_progress(pimpl_->total_output, pimpl_->header.uncompressed_size);
                    }

                    if (result->finished()) {
                        pimpl_->current_state = impl::state::DONE;
                        return stream_result::done(total_read, total_written);
                    }

                    // Propagate the status from the inner decompressor
                    return stream_result{total_read, total_written, result->status};
                }

                case impl::state::DONE:
                    return stream_result::done(total_read, total_written);
            }
        }
    }

    void szdd_decompressor::reset() {
        pimpl_->reset_state();
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
