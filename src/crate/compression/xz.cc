#include <crate/compression/xz.hh>
#include <lzma.h>

namespace crate {

// ============================================================================
// xz_decompressor (XZ container format)
// ============================================================================

struct xz_decompressor::impl {
    lzma_stream stream = LZMA_STREAM_INIT;
    bool initialized = false;
    bool finished = false;

    impl() {
        init();
    }

    ~impl() {
        if (initialized) {
            lzma_end(&stream);
        }
    }

    void init() {
        if (initialized) {
            lzma_end(&stream);
        }
        stream = LZMA_STREAM_INIT;
        // UINT64_MAX for memory limit = no limit
        lzma_ret ret = lzma_stream_decoder(&stream, UINT64_MAX, LZMA_CONCATENATED);
        initialized = (ret == LZMA_OK);
        finished = false;
    }
};

xz_decompressor::xz_decompressor()
    : pimpl_(std::make_unique<impl>()) {
}

xz_decompressor::~xz_decompressor() = default;

result_t<stream_result> xz_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    if (!pimpl_->initialized) {
        return std::unexpected(error{error_code::CorruptData, "xz initialization failed"});
    }

    if (pimpl_->finished) {
        return stream_result::done(0, 0);
    }

    pimpl_->stream.next_in = input.data();
    pimpl_->stream.avail_in = input.size();
    pimpl_->stream.next_out = output.data();
    pimpl_->stream.avail_out = output.size();

    lzma_action action = input_finished ? LZMA_FINISH : LZMA_RUN;
    lzma_ret ret = lzma_code(&pimpl_->stream, action);

    size_t bytes_read = input.size() - pimpl_->stream.avail_in;
    size_t bytes_written = output.size() - pimpl_->stream.avail_out;

    if (ret == LZMA_STREAM_END) {
        pimpl_->finished = true;
        if (bytes_written > 0) {
            report_progress(bytes_written, 0);
        }
        return stream_result::done(bytes_read, bytes_written);
    }

    if (ret != LZMA_OK) {
        const char* err_msg = "unknown error";
        switch (ret) {
            case LZMA_MEM_ERROR: err_msg = "memory allocation failed"; break;
            case LZMA_FORMAT_ERROR: err_msg = "invalid format"; break;
            case LZMA_OPTIONS_ERROR: err_msg = "unsupported options"; break;
            case LZMA_DATA_ERROR: err_msg = "data corruption"; break;
            case LZMA_BUF_ERROR: err_msg = "buffer error"; break;
            default: break;
        }
        return std::unexpected(error{error_code::CorruptData,
            std::string("xz decompression failed: ") + err_msg});
    }

    if (bytes_written > 0) {
        report_progress(bytes_written, 0);
    }

    if (pimpl_->stream.avail_out == 0) {
        return stream_result::need_output(bytes_read, bytes_written);
    }
    return stream_result::need_input(bytes_read, bytes_written);
}

void xz_decompressor::reset() {
    pimpl_->init();
}

// ============================================================================
// lzma_decompressor (raw LZMA without container)
// ============================================================================

struct lzma_decompressor::impl {
    lzma_stream stream = LZMA_STREAM_INIT;
    bool initialized = false;
    bool finished = false;

    impl() {
        init();
    }

    ~impl() {
        if (initialized) {
            lzma_end(&stream);
        }
    }

    void init() {
        if (initialized) {
            lzma_end(&stream);
        }
        stream = LZMA_STREAM_INIT;
        // UINT64_MAX for memory limit = no limit
        lzma_ret ret = lzma_alone_decoder(&stream, UINT64_MAX);
        initialized = (ret == LZMA_OK);
        finished = false;
    }
};

lzma_decompressor::lzma_decompressor()
    : pimpl_(std::make_unique<impl>()) {
}

lzma_decompressor::~lzma_decompressor() = default;

result_t<stream_result> lzma_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    if (!pimpl_->initialized) {
        return std::unexpected(error{error_code::CorruptData, "lzma initialization failed"});
    }

    if (pimpl_->finished) {
        return stream_result::done(0, 0);
    }

    pimpl_->stream.next_in = input.data();
    pimpl_->stream.avail_in = input.size();
    pimpl_->stream.next_out = output.data();
    pimpl_->stream.avail_out = output.size();

    lzma_action action = input_finished ? LZMA_FINISH : LZMA_RUN;
    lzma_ret ret = lzma_code(&pimpl_->stream, action);

    size_t bytes_read = input.size() - pimpl_->stream.avail_in;
    size_t bytes_written = output.size() - pimpl_->stream.avail_out;

    if (ret == LZMA_STREAM_END) {
        pimpl_->finished = true;
        if (bytes_written > 0) {
            report_progress(bytes_written, 0);
        }
        return stream_result::done(bytes_read, bytes_written);
    }

    if (ret != LZMA_OK) {
        const char* err_msg = "unknown error";
        switch (ret) {
            case LZMA_MEM_ERROR: err_msg = "memory allocation failed"; break;
            case LZMA_FORMAT_ERROR: err_msg = "invalid format"; break;
            case LZMA_OPTIONS_ERROR: err_msg = "unsupported options"; break;
            case LZMA_DATA_ERROR: err_msg = "data corruption"; break;
            case LZMA_BUF_ERROR: err_msg = "buffer error"; break;
            default: break;
        }
        return std::unexpected(error{error_code::CorruptData,
            std::string("lzma decompression failed: ") + err_msg});
    }

    if (bytes_written > 0) {
        report_progress(bytes_written, 0);
    }

    if (pimpl_->stream.avail_out == 0) {
        return stream_result::need_output(bytes_read, bytes_written);
    }
    return stream_result::need_input(bytes_read, bytes_written);
}

void lzma_decompressor::reset() {
    pimpl_->init();
}

} // namespace crate
