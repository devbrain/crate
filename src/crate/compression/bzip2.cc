#include <crate/compression/bzip2.hh>
#include <bzlib.h>

namespace crate {

struct bzip2_decompressor::impl {
    bz_stream stream{};
    bool initialized = false;
    bool finished = false;

    impl() {
        init();
    }

    ~impl() {
        if (initialized) {
            BZ2_bzDecompressEnd(&stream);
        }
    }

    void init() {
        if (initialized) {
            BZ2_bzDecompressEnd(&stream);
        }
        stream = {};
        // verbosity=0, small=0 (use more memory for speed)
        initialized = (BZ2_bzDecompressInit(&stream, 0, 0) == BZ_OK);
        finished = false;
    }
};

bzip2_decompressor::bzip2_decompressor()
    : pimpl_(std::make_unique<impl>()) {
}

bzip2_decompressor::~bzip2_decompressor() = default;

result_t<stream_result> bzip2_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    if (!pimpl_->initialized) {
        return crate::make_unexpected(error{error_code::CorruptData, "bzip2 initialization failed"});
    }

    if (pimpl_->finished) {
        return stream_result::done(0, 0);
    }

    pimpl_->stream.next_in = reinterpret_cast<char*>(const_cast<u8*>(input.data()));
    pimpl_->stream.avail_in = static_cast<unsigned int>(input.size());
    pimpl_->stream.next_out = reinterpret_cast<char*>(output.data());
    pimpl_->stream.avail_out = static_cast<unsigned int>(output.size());

    int ret = BZ2_bzDecompress(&pimpl_->stream);

    size_t bytes_read = input.size() - pimpl_->stream.avail_in;
    size_t bytes_written = output.size() - pimpl_->stream.avail_out;

    if (ret == BZ_STREAM_END) {
        pimpl_->finished = true;
        if (bytes_written > 0) {
            report_progress(bytes_written, 0);
        }
        return stream_result::done(bytes_read, bytes_written);
    }

    if (ret != BZ_OK) {
        const char* err_msg = "unknown error";
        switch (ret) {
            case BZ_PARAM_ERROR: err_msg = "parameter error"; break;
            case BZ_DATA_ERROR: err_msg = "data integrity error"; break;
            case BZ_DATA_ERROR_MAGIC: err_msg = "invalid magic number"; break;
            case BZ_MEM_ERROR: err_msg = "memory allocation failed"; break;
        }
        return crate::make_unexpected(error{error_code::CorruptData,
            std::string("bzip2 decompression failed: ") + err_msg});
    }

    // BZ_OK - continue decompressing
    if (input_finished && bytes_read == 0 && bytes_written == 0) {
        return crate::make_unexpected(error{error_code::CorruptData, "Unexpected end of bzip2 input"});
    }

    if (bytes_written > 0) {
        report_progress(bytes_written, 0);
    }

    // If output buffer is full, we need more output space
    if (pimpl_->stream.avail_out == 0) {
        return stream_result::need_output(bytes_read, bytes_written);
    }
    return stream_result::need_input(bytes_read, bytes_written);
}

void bzip2_decompressor::reset() {
    pimpl_->init();
}

} // namespace crate
