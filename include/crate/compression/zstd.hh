#pragma once

#include <crate/core/decompressor.hh>

namespace crate {

/// Zstandard (zstd) streaming decompressor
/// Decompresses zstd-compressed data (.zst files)
class CRATE_EXPORT zstd_decompressor : public decompressor {
public:
    zstd_decompressor();
    ~zstd_decompressor() override;

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    void reset() override;

private:
    struct impl;
    std::unique_ptr<impl> pimpl_;
};

} // namespace crate
