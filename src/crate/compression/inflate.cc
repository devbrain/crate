// Public header
#include <crate/compression/inflate.hh>

#include <algorithm>
#include <cstdint>
#include <vector>

// Configure miniz before including
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_EXPORT
#include <miniz.h>

namespace crate {

// ============================================================================
// inflate_decompressor (raw DEFLATE)
// ============================================================================

struct inflate_decompressor::impl {
    tinfl_decompressor inflator{};
    bool finished = false;

    impl() {
        tinfl_init(&inflator);
    }
};

inflate_decompressor::inflate_decompressor()
    : pimpl_(std::make_unique<impl>()) {
}

inflate_decompressor::~inflate_decompressor() = default;

result_t<stream_result> inflate_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    if (pimpl_->finished) {
        return stream_result::done(0, 0);
    }

    size_t in_bytes = input.size();
    size_t out_bytes = output.size();

    mz_uint32 flags = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
    if (!input_finished) {
        flags |= TINFL_FLAG_HAS_MORE_INPUT;
    }

    auto status = tinfl_decompress(
        &pimpl_->inflator,
        input.data(),
        &in_bytes,
        output.data(),
        output.data(),
        &out_bytes,
        flags
    );

    if (status < TINFL_STATUS_DONE && status != TINFL_STATUS_NEEDS_MORE_INPUT) {
        return crate::make_unexpected(error{error_code::CorruptData, "DEFLATE decompression failed"});
    }

    if (status == TINFL_STATUS_DONE) {
        pimpl_->finished = true;
        if (out_bytes > 0) {
            report_progress(out_bytes, 0);
        }
        return stream_result::done(in_bytes, out_bytes);
    }

    if (out_bytes > 0) {
        report_progress(out_bytes, 0);
    }

    // TINFL_STATUS_HAS_MORE_OUTPUT means output buffer is full
    if (status == TINFL_STATUS_HAS_MORE_OUTPUT) {
        return stream_result::need_output(in_bytes, out_bytes);
    }
    return stream_result::need_input(in_bytes, out_bytes);
}

void inflate_decompressor::reset() {
    tinfl_init(&pimpl_->inflator);
    pimpl_->finished = false;
}

// ============================================================================
// zlib_decompressor (DEFLATE with zlib header)
// ============================================================================

struct zlib_decompressor::impl {
    mz_stream stream{};
    bool initialized = false;
    bool finished = false;

    impl() {
        init();
    }

    ~impl() {
        if (initialized) {
            mz_inflateEnd(&stream);
        }
    }

    void init() {
        if (initialized) {
            mz_inflateEnd(&stream);
        }
        stream = {};
        initialized = (mz_inflateInit(&stream) == MZ_OK);
        finished = false;
    }
};

zlib_decompressor::zlib_decompressor()
    : pimpl_(std::make_unique<impl>()) {
}

zlib_decompressor::~zlib_decompressor() = default;

result_t<stream_result> zlib_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    if (!pimpl_->initialized) {
        return crate::make_unexpected(error{error_code::CorruptData, "zlib initialization failed"});
    }

    if (pimpl_->finished) {
        return stream_result::done(0, 0);
    }

    // Guard against >4GB buffers which would truncate when cast to mz_uint32
    constexpr size_t max_chunk = static_cast<size_t>(UINT32_MAX);
    size_t in_size = std::min(input.size(), max_chunk);
    size_t out_size = std::min(output.size(), max_chunk);

    pimpl_->stream.next_in = input.data();
    pimpl_->stream.avail_in = static_cast<mz_uint32>(in_size);
    pimpl_->stream.next_out = output.data();
    pimpl_->stream.avail_out = static_cast<mz_uint32>(out_size);

    int flush = input_finished ? MZ_FINISH : MZ_NO_FLUSH;
    int status = mz_inflate(&pimpl_->stream, flush);

    size_t bytes_read = in_size - pimpl_->stream.avail_in;
    size_t bytes_written = out_size - pimpl_->stream.avail_out;

    if (status == MZ_STREAM_END) {
        pimpl_->finished = true;
        if (bytes_written > 0) {
            report_progress(bytes_written, 0);
        }
        return stream_result::done(bytes_read, bytes_written);
    }

    if (status != MZ_OK && status != MZ_BUF_ERROR) {
        return crate::make_unexpected(error{error_code::CorruptData, "zlib decompression failed"});
    }

    if (bytes_written > 0) {
        report_progress(bytes_written, 0);
    }

    if (pimpl_->stream.avail_out == 0) {
        return stream_result::need_output(bytes_read, bytes_written);
    }
    return stream_result::need_input(bytes_read, bytes_written);
}

void zlib_decompressor::reset() {
    pimpl_->init();
}

// ============================================================================
// gzip_decompressor (DEFLATE with gzip header)
//
// Miniz does not support window_bits + 16 for gzip auto-detection.
// We manually parse the gzip header per RFC 1952, then use raw inflate
// (negative window_bits) for the compressed data.
//
// The header is parsed directly from the caller's input span. Once parsed,
// the post-header bytes are forwarded to inflate in the same call, and
// bytes_read reflects header_size + inflate_consumed so the caller's
// buffer management (e.g. decompressing_streambuf) works correctly.
// ============================================================================

struct gzip_decompressor::impl {
    mz_stream stream{};
    bool inflator_initialized = false;
    bool header_parsed = false;
    bool finished = false;

    // Small buffer for the rare case where the header spans multiple calls
    std::vector<u8> header_buf;
    // Leftover bytes from header parsing (only used when header spans calls)
    std::vector<u8> leftover;
    size_t leftover_pos = 0;

    impl() = default;

    ~impl() {
        if (inflator_initialized) mz_inflateEnd(&stream);
    }

    void init() {
        if (inflator_initialized) {
            mz_inflateEnd(&stream);
            inflator_initialized = false;
        }
        stream = {};
        header_parsed = false;
        finished = false;
        header_buf.clear();
        leftover.clear();
        leftover_pos = 0;
    }

    // Parse gzip header from data[0..len). Returns header size,
    // 0 if more data needed, SIZE_MAX on error.
    static size_t parse_header(const u8* p, size_t len) {
        if (len < 10) return 0;
        if (p[0] != 0x1F || p[1] != 0x8B || p[2] != 8) return SIZE_MAX;

        u8 flags = p[3];
        size_t pos = 10;

        if (flags & 0x04) { // FEXTRA
            if (pos + 2 > len) return 0;
            size_t xlen = static_cast<size_t>(p[pos]) |
                          (static_cast<size_t>(p[pos + 1]) << 8);
            pos += 2;
            if (pos + xlen > len) return 0;
            pos += xlen;
        }
        if (flags & 0x08) { // FNAME
            while (pos < len && p[pos] != 0) ++pos;
            if (pos >= len) return 0;
            ++pos;
        }
        if (flags & 0x10) { // FCOMMENT
            while (pos < len && p[pos] != 0) ++pos;
            if (pos >= len) return 0;
            ++pos;
        }
        if (flags & 0x02) { // FHCRC
            if (pos + 2 > len) return 0;
            pos += 2;
        }
        return pos;
    }

    bool init_raw_inflate() {
        stream = {};
        inflator_initialized = (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) == MZ_OK);
        return inflator_initialized;
    }

    // Run inflate. Returns (consumed, written, mz_status).
    struct inflate_result { size_t consumed, written; int status; };

    inflate_result do_inflate(const u8* in, size_t in_len,
                              u8* out, size_t out_len) {
        constexpr size_t max_chunk = static_cast<size_t>(UINT32_MAX);
        auto in_sz = std::min(in_len, max_chunk);
        auto out_sz = std::min(out_len, max_chunk);

        stream.next_in = in;
        stream.avail_in = static_cast<mz_uint32>(in_sz);
        stream.next_out = out;
        stream.avail_out = static_cast<mz_uint32>(out_sz);

        // Always use MZ_NO_FLUSH for raw inflate within gzip.
        // The deflate stream has its own end markers; MZ_STREAM_END
        // is returned naturally. Using MZ_FINISH corrupts state when
        // the output buffer is too small to hold all decompressed data.
        int st = mz_inflate(&stream, MZ_NO_FLUSH);
        return {in_sz - stream.avail_in, out_sz - stream.avail_out, st};
    }

    // Convert inflate result to stream_result
    result_t<stream_result> to_stream_result(inflate_result r, size_t extra_read) {
        if (r.status == MZ_STREAM_END) {
            finished = true;
            return stream_result::done(r.consumed + extra_read, r.written);
        }
        if (r.status != MZ_OK && r.status != MZ_BUF_ERROR) {
            return crate::make_unexpected(error{error_code::CorruptData, "gzip decompression failed"});
        }
        if (stream.avail_out == 0) {
            return stream_result::need_output(r.consumed + extra_read, r.written);
        }
        return stream_result::need_input(r.consumed + extra_read, r.written);
    }
};

gzip_decompressor::gzip_decompressor()
    : pimpl_(std::make_unique<impl>()) {
}

gzip_decompressor::~gzip_decompressor() = default;

result_t<stream_result> gzip_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    if (pimpl_->finished) return stream_result::done(0, 0);

    // === Parse header (first call or buffered multi-call) ===
    if (!pimpl_->header_parsed) {
        // Fast path: try to parse header directly from input span
        if (pimpl_->header_buf.empty()) {
            size_t hdr_len = impl::parse_header(input.data(), input.size());
            if (hdr_len == SIZE_MAX) {
                return crate::make_unexpected(error{error_code::CorruptData, "invalid gzip header"});
            }
            if (hdr_len > 0) {
                // Header fits in this single call — no buffering needed
                if (!pimpl_->init_raw_inflate()) {
                    return crate::make_unexpected(error{error_code::CorruptData, "inflate initialization failed"});
                }
                pimpl_->header_parsed = true;

                // Inflate the post-header bytes from this same input
                const u8* deflate_data = input.data() + hdr_len;
                size_t deflate_len = input.size() - hdr_len;

                auto r = pimpl_->do_inflate(deflate_data, deflate_len,
                                            output.data(), output.size());

                if (r.written > 0) report_progress(r.written, 0);

                // bytes_read = header + inflate_consumed
                auto result = pimpl_->to_stream_result(r, hdr_len);
                return result;
            }
            // Header incomplete — fall through to buffered path
        }

        // Buffered path: accumulate input
        pimpl_->header_buf.insert(pimpl_->header_buf.end(),
                                  input.data(), input.data() + input.size());

        size_t hdr_len = impl::parse_header(pimpl_->header_buf.data(), pimpl_->header_buf.size());
        if (hdr_len == SIZE_MAX) {
            return crate::make_unexpected(error{error_code::CorruptData, "invalid gzip header"});
        }
        if (hdr_len == 0) {
            if (input_finished) {
                return crate::make_unexpected(error{error_code::CorruptData, "truncated gzip header"});
            }
            return stream_result::need_input(input.size(), 0);
        }

        if (!pimpl_->init_raw_inflate()) {
            return crate::make_unexpected(error{error_code::CorruptData, "inflate initialization failed"});
        }
        pimpl_->header_parsed = true;

        // Stash post-header bytes as leftover
        size_t remaining = pimpl_->header_buf.size() - hdr_len;
        if (remaining > 0) {
            pimpl_->leftover.assign(
                pimpl_->header_buf.data() + hdr_len,
                pimpl_->header_buf.data() + pimpl_->header_buf.size());
            pimpl_->leftover_pos = 0;
        }
        pimpl_->header_buf.clear();

        if (pimpl_->leftover.empty()) {
            return stream_result::need_input(input.size(), 0);
        }

        // Inflate from leftover
        size_t lo_avail = pimpl_->leftover.size() - pimpl_->leftover_pos;
        auto r = pimpl_->do_inflate(
            pimpl_->leftover.data() + pimpl_->leftover_pos, lo_avail,
            output.data(), output.size());

        pimpl_->leftover_pos += r.consumed;
        if (pimpl_->leftover_pos >= pimpl_->leftover.size()) {
            pimpl_->leftover.clear();
            pimpl_->leftover_pos = 0;
        }

        if (r.written > 0) report_progress(r.written, 0);

        if (r.status == MZ_STREAM_END) {
            pimpl_->finished = true;
            return stream_result::done(input.size(), r.written);
        }
        if (r.status != MZ_OK && r.status != MZ_BUF_ERROR) {
            return crate::make_unexpected(error{error_code::CorruptData, "gzip decompression failed"});
        }
        return stream_result::need_input(input.size(), r.written);
    }

    // === Drain leftover bytes from buffered header parse ===
    if (!pimpl_->leftover.empty()) {
        size_t lo_avail = pimpl_->leftover.size() - pimpl_->leftover_pos;
        auto r = pimpl_->do_inflate(
            pimpl_->leftover.data() + pimpl_->leftover_pos, lo_avail,
            output.data(), output.size());

        pimpl_->leftover_pos += r.consumed;
        if (pimpl_->leftover_pos >= pimpl_->leftover.size()) {
            pimpl_->leftover.clear();
            pimpl_->leftover_pos = 0;
        }

        if (r.written > 0) report_progress(r.written, 0);

        if (r.status == MZ_STREAM_END) {
            pimpl_->finished = true;
            return stream_result::done(0, r.written);
        }
        if (r.status != MZ_OK && r.status != MZ_BUF_ERROR) {
            return crate::make_unexpected(error{error_code::CorruptData, "gzip decompression failed"});
        }
        return stream_result::need_input(0, r.written);
    }

    // === Normal inflate from external input ===
    auto r = pimpl_->do_inflate(
        input.data(), input.size(),
        output.data(), output.size());

    if (r.written > 0) report_progress(r.written, 0);

    auto result = pimpl_->to_stream_result(r, 0);
    return result;
}

void gzip_decompressor::reset() {
    pimpl_->init();
}

} // namespace crate
