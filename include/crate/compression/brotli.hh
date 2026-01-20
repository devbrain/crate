#pragma once

#include <crate/core/decompressor.hh>

namespace crate {

/// Brotli streaming decompressor
/// Decompresses brotli-compressed data (.br files)
class CRATE_EXPORT brotli_decompressor : public decompressor {
public:
    brotli_decompressor();
    ~brotli_decompressor() override;

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    [[nodiscard]] bool supports_streaming() const override { return true; }

    void reset() override;

private:
    struct impl;
    std::unique_ptr<impl> pimpl_;
};

} // namespace crate
