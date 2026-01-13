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

    impl() {
        tinfl_init(&inflator);
    }
};

inflate_decompressor::inflate_decompressor()
    : pimpl_(std::make_unique<impl>()) {
}

inflate_decompressor::~inflate_decompressor() = default;

result_t<size_t> inflate_decompressor::decompress(byte_span input, mutable_byte_span output) {
    size_t in_bytes = input.size();
    size_t out_bytes = output.size();

    auto status = tinfl_decompress(
        &pimpl_->inflator,
        input.data(),
        &in_bytes,
        output.data(),
        output.data(),
        &out_bytes,
        TINFL_FLAG_PARSE_ZLIB_HEADER * 0 |  // Raw deflate, no header
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF
    );

    if (status < TINFL_STATUS_DONE) {
        return std::unexpected(error{error_code::CorruptData, "DEFLATE decompression failed"});
    }

    return out_bytes;
}

void inflate_decompressor::reset() {
    tinfl_init(&pimpl_->inflator);
}

// ============================================================================
// zlib_decompressor (DEFLATE with zlib header)
// ============================================================================

struct zlib_decompressor::impl {
    mz_stream stream{};
    bool initialized = false;

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
    }
};

zlib_decompressor::zlib_decompressor()
    : pimpl_(std::make_unique<impl>()) {
}

zlib_decompressor::~zlib_decompressor() = default;

result_t<size_t> zlib_decompressor::decompress(byte_span input, mutable_byte_span output) {
    if (!pimpl_->initialized) {
        return std::unexpected(error{error_code::CorruptData, "zlib initialization failed"});
    }

    pimpl_->stream.next_in = input.data();
    pimpl_->stream.avail_in = static_cast<mz_uint32>(input.size());
    pimpl_->stream.next_out = output.data();
    pimpl_->stream.avail_out = static_cast<mz_uint32>(output.size());

    int status = mz_inflate(&pimpl_->stream, MZ_FINISH);

    if (status != MZ_STREAM_END && status != MZ_OK) {
        return std::unexpected(error{error_code::CorruptData, "zlib decompression failed"});
    }

    return pimpl_->stream.total_out;
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
    }
};

gzip_decompressor::gzip_decompressor()
    : pimpl_(std::make_unique<impl>()) {
}

gzip_decompressor::~gzip_decompressor() = default;

result_t<size_t> gzip_decompressor::decompress(byte_span input, mutable_byte_span output) {
    if (!pimpl_->initialized) {
        return std::unexpected(error{error_code::CorruptData, "gzip initialization failed"});
    }

    pimpl_->stream.next_in = input.data();
    pimpl_->stream.avail_in = static_cast<mz_uint32>(input.size());
    pimpl_->stream.next_out = output.data();
    pimpl_->stream.avail_out = static_cast<mz_uint32>(output.size());

    int status = mz_inflate(&pimpl_->stream, MZ_FINISH);

    if (status != MZ_STREAM_END && status != MZ_OK) {
        return std::unexpected(error{error_code::CorruptData, "gzip decompression failed"});
    }

    return pimpl_->stream.total_out;
}

void gzip_decompressor::reset() {
    pimpl_->init();
}

} // namespace crate
