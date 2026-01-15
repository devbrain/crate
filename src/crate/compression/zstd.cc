#include <crate/compression/zstd.hh>
#include <zstd.h>

namespace crate {

struct zstd_decompressor::impl {
    ZSTD_DCtx* ctx = nullptr;
    bool frame_complete = false;

    impl() {
        ctx = ZSTD_createDCtx();
    }

    ~impl() {
        if (ctx) {
            ZSTD_freeDCtx(ctx);
        }
    }

    void reset_ctx() {
        if (ctx) {
            ZSTD_DCtx_reset(ctx, ZSTD_reset_session_only);
        }
        frame_complete = false;
    }
};

zstd_decompressor::zstd_decompressor()
    : pimpl_(std::make_unique<impl>()) {
}

zstd_decompressor::~zstd_decompressor() = default;

result_t<stream_result> zstd_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    [[maybe_unused]] bool input_finished
) {
    if (!pimpl_->ctx) {
        return std::unexpected(error{error_code::CorruptData, "zstd context initialization failed"});
    }

    if (pimpl_->frame_complete) {
        return stream_result::done(0, 0);
    }

    ZSTD_inBuffer in_buf = {input.data(), input.size(), 0};
    ZSTD_outBuffer out_buf = {output.data(), output.size(), 0};

    size_t ret = ZSTD_decompressStream(pimpl_->ctx, &out_buf, &in_buf);

    if (ZSTD_isError(ret)) {
        return std::unexpected(error{error_code::CorruptData,
            std::string("zstd decompression failed: ") + ZSTD_getErrorName(ret)});
    }

    // ret == 0 means frame is complete
    bool finished = (ret == 0);
    if (finished) {
        pimpl_->frame_complete = true;
    }

    // Report progress
    if (out_buf.pos > 0) {
        report_progress(out_buf.pos, 0);
    }

    if (finished) {
        return stream_result::done(in_buf.pos, out_buf.pos);
    }
    // If output buffer is full, we need more output space
    if (out_buf.pos == output.size()) {
        return stream_result::need_output(in_buf.pos, out_buf.pos);
    }
    // Otherwise, we need more input
    return stream_result::need_input(in_buf.pos, out_buf.pos);
}

void zstd_decompressor::reset() {
    pimpl_->reset_ctx();
}

} // namespace crate
