// Public header
#include <crate/compression/inflate.hh>

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
        return std::unexpected(error{error_code::CorruptData, "DEFLATE decompression failed"});
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
        return std::unexpected(error{error_code::CorruptData, "zlib initialization failed"});
    }

    if (pimpl_->finished) {
        return stream_result::done(0, 0);
    }

    pimpl_->stream.next_in = input.data();
    pimpl_->stream.avail_in = static_cast<mz_uint32>(input.size());
    pimpl_->stream.next_out = output.data();
    pimpl_->stream.avail_out = static_cast<mz_uint32>(output.size());

    int flush = input_finished ? MZ_FINISH : MZ_NO_FLUSH;
    int status = mz_inflate(&pimpl_->stream, flush);

    size_t bytes_read = input.size() - pimpl_->stream.avail_in;
    size_t bytes_written = output.size() - pimpl_->stream.avail_out;

    if (status == MZ_STREAM_END) {
        pimpl_->finished = true;
        if (bytes_written > 0) {
            report_progress(bytes_written, 0);
        }
        return stream_result::done(bytes_read, bytes_written);
    }

    if (status != MZ_OK && status != MZ_BUF_ERROR) {
        return std::unexpected(error{error_code::CorruptData, "zlib decompression failed"});
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
// ============================================================================

struct gzip_decompressor::impl {
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
        // window_bits = 15 + 16 for gzip format
        initialized = (mz_inflateInit2(&stream, MZ_DEFAULT_WINDOW_BITS + 16) == MZ_OK);
        finished = false;
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
    if (!pimpl_->initialized) {
        return std::unexpected(error{error_code::CorruptData, "gzip initialization failed"});
    }

    if (pimpl_->finished) {
        return stream_result::done(0, 0);
    }

    pimpl_->stream.next_in = input.data();
    pimpl_->stream.avail_in = static_cast<mz_uint32>(input.size());
    pimpl_->stream.next_out = output.data();
    pimpl_->stream.avail_out = static_cast<mz_uint32>(output.size());

    int flush = input_finished ? MZ_FINISH : MZ_NO_FLUSH;
    int status = mz_inflate(&pimpl_->stream, flush);

    size_t bytes_read = input.size() - pimpl_->stream.avail_in;
    size_t bytes_written = output.size() - pimpl_->stream.avail_out;

    if (status == MZ_STREAM_END) {
        pimpl_->finished = true;
        if (bytes_written > 0) {
            report_progress(bytes_written, 0);
        }
        return stream_result::done(bytes_read, bytes_written);
    }

    if (status != MZ_OK && status != MZ_BUF_ERROR) {
        return std::unexpected(error{error_code::CorruptData, "gzip decompression failed"});
    }

    if (bytes_written > 0) {
        report_progress(bytes_written, 0);
    }

    if (pimpl_->stream.avail_out == 0) {
        return stream_result::need_output(bytes_read, bytes_written);
    }
    return stream_result::need_input(bytes_read, bytes_written);
}

void gzip_decompressor::reset() {
    pimpl_->init();
}

} // namespace crate
