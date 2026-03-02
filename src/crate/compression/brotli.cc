#include <crate/compression/brotli.hh>
#include <brotli/decode.h>

namespace crate {

struct brotli_decompressor::impl {
    BrotliDecoderState* state = nullptr;
    bool finished = false;

    impl() {
        state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    }

    ~impl() {
        if (state) {
            BrotliDecoderDestroyInstance(state);
        }
    }

    void reset_state() {
        if (state) {
            BrotliDecoderDestroyInstance(state);
        }
        state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
        finished = false;
    }
};

brotli_decompressor::brotli_decompressor()
    : pimpl_(std::make_unique<impl>()) {
}

brotli_decompressor::~brotli_decompressor() = default;

result_t<stream_result> brotli_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    if (!pimpl_->state) {
        return crate::make_unexpected(error{error_code::CorruptData, "brotli state initialization failed"});
    }

    if (pimpl_->finished) {
        return stream_result::done(0, 0);
    }

    const uint8_t* next_in = input.data();
    size_t available_in = input.size();
    uint8_t* next_out = output.data();
    size_t available_out = output.size();
    size_t total_out = 0;

    BrotliDecoderResult result = BrotliDecoderDecompressStream(
        pimpl_->state,
        &available_in, &next_in,
        &available_out, &next_out,
        &total_out
    );

    size_t bytes_read = input.size() - available_in;
    size_t bytes_written = output.size() - available_out;

    if (result == BROTLI_DECODER_RESULT_SUCCESS) {
        pimpl_->finished = true;
        if (bytes_written > 0) {
            report_progress(bytes_written, 0);
        }
        return stream_result::done(bytes_read, bytes_written);
    }

    if (result == BROTLI_DECODER_RESULT_ERROR) {
        BrotliDecoderErrorCode err = BrotliDecoderGetErrorCode(pimpl_->state);
        return crate::make_unexpected(error{error_code::CorruptData,
            std::string("brotli decompression failed: ") + BrotliDecoderErrorString(err)});
    }

    if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && input_finished && available_in == 0) {
        return crate::make_unexpected(error{error_code::CorruptData, "Unexpected end of brotli input"});
    }

    // Report progress before returning status
    if (bytes_written > 0) {
        report_progress(bytes_written, 0);
    }

    // Use brotli's explicit status
    if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
        return stream_result::need_output(bytes_read, bytes_written);
    }
    return stream_result::need_input(bytes_read, bytes_written);
}

void brotli_decompressor::reset() {
    pimpl_->reset_state();
}

} // namespace crate
