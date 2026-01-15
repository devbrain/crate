#pragma once

#include <crate/core/decompressor.hh>

namespace crate {

/// Bzip2 streaming decompressor
/// Decompresses bzip2-compressed data (.bz2 files)
class CRATE_EXPORT bzip2_decompressor : public decompressor {
public:
    bzip2_decompressor();
    ~bzip2_decompressor() override;

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
