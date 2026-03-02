#include <crate/compression/kwaj.hh>
#include "crate/compression/lzss.hh"
#include "crate/compression/mszip.hh"
#include <algorithm>
#include <cstring>
#include <vector>

namespace crate {

    struct kwaj_decompressor::impl {
        enum class state : u8 {
            READ_HEADER,
            DECOMPRESS_DATA,
            DONE
        };

        state current_state = state::READ_HEADER;

        szdd_lzss_decompressor szdd_lzss;
        kwaj_lzss_decompressor kwaj_lzss;
        mszip_decompressor mszip;

        std::vector<u8> header_buf;
        size_t header_bytes = 0;
        size_t header_target = 0;
        bool header_target_known = false;

        kwaj::header header{};
        kwaj::method method = kwaj::method::NONE;
        size_t expected_output = 0;

        std::vector<u8> pending_input;
        size_t pending_pos = 0;

        std::vector<u8> buffered_input;
        std::vector<u8> buffered_output;
        size_t buffered_output_pos = 0;
        bool buffered_ready = false;

        size_t total_output = 0;

        void reset_state() {
            current_state = state::READ_HEADER;
            header_buf.clear();
            header_bytes = 0;
            header_target = 0;
            header_target_known = false;
            header = {};
            method = kwaj::method::NONE;
            expected_output = 0;
            pending_input.clear();
            pending_pos = 0;
            buffered_input.clear();
            buffered_output.clear();
            buffered_output_pos = 0;
            buffered_ready = false;
            total_output = 0;
            szdd_lzss.reset();
            kwaj_lzss.reset();
            mszip.reset();
        }
    };

    kwaj_decompressor::kwaj_decompressor() : pimpl_(std::make_unique<impl>()) {}

    kwaj_decompressor::~kwaj_decompressor() = default;

    result_t<kwaj::header> kwaj_decompressor::parse_header(byte_span data) {
        if (data.size() < 14) {
            return crate::make_unexpected(error{error_code::TruncatedArchive});
        }

        // Check signatures - KWAJ signature + variant marker (0x33 or 0xD1)
        if (std::memcmp(data.data(), kwaj::SIGNATURE1, 4) != 0 ||
            std::memcmp(data.data() + 4, kwaj::SIGNATURE2_PART, 3) != 0) {
            return crate::make_unexpected(error{
                error_code::InvalidSignature,
                "Not a valid KWAJ file"
            });
        }
        // Byte 7 can be 0x33 or 0xD1 (two known variants)
        u8 variant = data[7];
        if (variant != 0x33 && variant != 0xD1) {
            return crate::make_unexpected(error{
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
                return crate::make_unexpected(error{error_code::TruncatedArchive});
            }
            header.uncompressed_len = read_u32_le(data.data() + pos);
            pos += 4;
        }

        if (header.flags & kwaj::HAS_UNKNOWN) {
            if (pos + 4 > data.size()) {
                return crate::make_unexpected(error{error_code::TruncatedArchive});
            }
            header.unknown = read_u32_le(data.data() + pos);
            pos += 4;
        }

        if (header.flags & kwaj::HAS_DECOMPRESSED_LEN) {
            if (pos + 4 > data.size()) {
                return crate::make_unexpected(error{error_code::TruncatedArchive});
            }
            header.decompressed_len = read_u32_le(data.data() + pos);
            pos += 4;
        }

        if (header.flags & kwaj::HAS_FILENAME) {
            // Filename is null-terminated, max 8 characters
            if (pos >= data.size()) {
                return crate::make_unexpected(error{error_code::TruncatedArchive});
            }
            size_t name_start = pos;
            while (pos < data.size() && data[pos] != 0) {
                pos++;
            }
            size_t name_len = pos - name_start;
            if (name_len > 8) {
                return crate::make_unexpected(error{
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
                    return crate::make_unexpected(error{
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

    result_t<stream_result> kwaj_decompressor::decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished
    ) {
        size_t total_read = 0;
        size_t total_written = 0;

        auto pending_remaining = [&]() -> size_t {
            if (pimpl_->pending_pos >= pimpl_->pending_input.size()) {
                pimpl_->pending_input.clear();
                pimpl_->pending_pos = 0;
                return 0;
            }
            return pimpl_->pending_input.size() - pimpl_->pending_pos;
        };

        auto pending_span = [&]() -> byte_span {
            size_t remaining = pending_remaining();
            if (remaining == 0) {
                return {};
            }
            return byte_span{
                pimpl_->pending_input.data() + pimpl_->pending_pos,
                remaining
            };
        };

        auto consume_pending = [&](size_t bytes) {
            pimpl_->pending_pos += bytes;
            pending_remaining();
        };

        auto output_remaining = [&]() -> size_t {
            return output.size() - total_written;
        };

        auto note_progress = [&](size_t bytes) {
            if (bytes == 0) {
                return;
            }
            pimpl_->total_output += bytes;
            report_progress(pimpl_->total_output, pimpl_->expected_output);
        };

        while (true) {
            switch (pimpl_->current_state) {
                case impl::state::READ_HEADER: {
                    if (pimpl_->header_bytes >= 8) {
                        if (std::memcmp(pimpl_->header_buf.data(), kwaj::SIGNATURE1, 4) != 0 ||
                            std::memcmp(pimpl_->header_buf.data() + 4, kwaj::SIGNATURE2_PART, 3) != 0) {
                            return crate::make_unexpected(error{
                                error_code::InvalidSignature,
                                "Not a valid KWAJ file"
                            });
                        }

                        u8 variant = pimpl_->header_buf[7];
                        if (variant != 0x33 && variant != 0xD1) {
                            return crate::make_unexpected(error{
                                error_code::InvalidSignature,
                                "Unknown KWAJ variant"
                            });
                        }
                    }

                    if (!pimpl_->header_target_known && pimpl_->header_bytes >= 14) {
                        pimpl_->header_target = read_u16_le(pimpl_->header_buf.data() + 10);
                        pimpl_->header_target_known = true;
                    }

                    size_t header_need = 14;
                    if (pimpl_->header_target_known && pimpl_->header_target > header_need) {
                        header_need = pimpl_->header_target;
                    }

                    if (pimpl_->header_bytes >= header_need) {
                        auto header_result = parse_header(byte_span{
                            pimpl_->header_buf.data(),
                            pimpl_->header_bytes
                        });
                        if (!header_result) {
                            return crate::make_unexpected(header_result.error());
                        }

                        pimpl_->header = *header_result;
                        pimpl_->method = static_cast<kwaj::method>(pimpl_->header.comp_method);
                        pimpl_->expected_output = pimpl_->header.decompressed_len > 0
                            ? pimpl_->header.decompressed_len
                            : pimpl_->header.uncompressed_len;

                        if (pimpl_->header_target_known && pimpl_->header_bytes > pimpl_->header_target) {
                            size_t extra = pimpl_->header_bytes - pimpl_->header_target;
                            pimpl_->pending_input.resize(extra);
                            std::memcpy(
                                pimpl_->pending_input.data(),
                                pimpl_->header_buf.data() + pimpl_->header_target,
                                extra
                            );
                            pimpl_->pending_pos = 0;
                        }

                        if (input_finished && total_read >= input.size() && pimpl_->pending_input.empty()) {
                            return crate::make_unexpected(error{error_code::TruncatedArchive});
                        }

                        pimpl_->current_state = impl::state::DECOMPRESS_DATA;
                        continue;
                    }

                    size_t available = input.size() - total_read;
                    if (available == 0) {
                        if (input_finished) {
                            return crate::make_unexpected(error{error_code::TruncatedArchive});
                        }
                        return stream_result::need_input(total_read, total_written);
                    }

                    size_t to_copy = std::min(header_need - pimpl_->header_bytes, available);
                    pimpl_->header_buf.resize(pimpl_->header_bytes + to_copy);
                    std::memcpy(
                        pimpl_->header_buf.data() + pimpl_->header_bytes,
                        input.data() + total_read,
                        to_copy
                    );
                    pimpl_->header_bytes += to_copy;
                    total_read += to_copy;
                    continue;
                }

                case impl::state::DECOMPRESS_DATA: {
                    switch (pimpl_->method) {
                        case kwaj::NONE:
                        case kwaj::XOR_FF: {
                            byte_span src = pending_span();
                            bool using_pending = !src.empty();
                            if (!using_pending) {
                                src = input.subspan(total_read);
                            }

                            if (src.empty()) {
                                if (input_finished) {
                                    pimpl_->current_state = impl::state::DONE;
                                    return stream_result::done(total_read, total_written);
                                }
                                return stream_result::need_input(total_read, total_written);
                            }

                            if (output_remaining() == 0) {
                                return stream_result::need_output(total_read, total_written);
                            }

                            size_t to_copy = std::min(src.size(), output_remaining());
                            if (pimpl_->method == kwaj::XOR_FF) {
                                for (size_t i = 0; i < to_copy; i++) {
                                    output[total_written + i] = static_cast<byte>(src[i] ^ 0xFF);
                                }
                            } else {
                                std::memcpy(output.data() + total_written, src.data(), to_copy);
                            }

                            if (using_pending) {
                                consume_pending(to_copy);
                            } else {
                                total_read += to_copy;
                            }

                            total_written += to_copy;
                            note_progress(to_copy);

                            if (to_copy < src.size()) {
                                return stream_result::need_output(total_read, total_written);
                            }

                            if (using_pending) {
                                continue;
                            }

                            if (total_read >= input.size()) {
                                if (input_finished) {
                                    pimpl_->current_state = impl::state::DONE;
                                    return stream_result::done(total_read, total_written);
                                }
                                return stream_result::need_input(total_read, total_written);
                            }
                            continue;
                        }

                        case kwaj::SZDD:
                        case kwaj::MSZIP:
                        case kwaj::LZH: {
                            decompressor& inner = pimpl_->method == kwaj::SZDD
                                ? static_cast<decompressor&>(pimpl_->szdd_lzss)
                                : (pimpl_->method == kwaj::MSZIP
                                    ? static_cast<decompressor&>(pimpl_->mszip)
                                    : static_cast<decompressor&>(pimpl_->kwaj_lzss));

                            byte_span src = pending_span();
                            bool using_pending = !src.empty();
                            if (!using_pending) {
                                src = input.subspan(total_read);
                            }

                            bool inner_finished = input_finished && !using_pending;
                            if (src.empty() && !inner_finished) {
                                return stream_result::need_input(total_read, total_written);
                            }

                            if (output_remaining() == 0) {
                                return stream_result::need_output(total_read, total_written);
                            }

                            auto result = inner.decompress_some(
                                src,
                                output.subspan(total_written),
                                inner_finished
                            );
                            if (!result) {
                                return crate::make_unexpected(result.error());
                            }

                            if (using_pending) {
                                consume_pending(result->bytes_read);
                            } else {
                                total_read += result->bytes_read;
                            }

                            total_written += result->bytes_written;
                            note_progress(result->bytes_written);

                            if (result->finished()) {
                                pimpl_->current_state = impl::state::DONE;
                                return stream_result::done(total_read, total_written);
                            }

                            if (result->status == decode_status::needs_more_output) {
                                return stream_result::need_output(total_read, total_written);
                            }

                            if (result->status == decode_status::needs_more_input) {
                                if (using_pending && pending_remaining() == 0) {
                                    continue;
                                }
                                if (!using_pending && total_read >= input.size()) {
                                    if (input_finished) {
                                        return stream_result{
                                            total_read,
                                            total_written,
                                            decode_status::needs_more_input
                                        };
                                    }
                                    return stream_result::need_input(total_read, total_written);
                                }
                            }

                            return stream_result{total_read, total_written, result->status};
                        }

                        default:
                            return crate::make_unexpected(error{
                                error_code::UnsupportedCompression,
                                "Unknown KWAJ compression method"
                            });
                    }
                }

                case impl::state::DONE:
                    return stream_result::done(total_read, total_written);
            }
        }
    }

    void kwaj_decompressor::reset() {
        pimpl_->reset_state();
    }

    result_t<size_t> kwaj_decompressor::decompress_xor(byte_span data, mutable_byte_span output) {
        if (output.size() < data.size()) {
            return crate::make_unexpected(error{error_code::OutputBufferOverflow});
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
