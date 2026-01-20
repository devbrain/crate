#pragma once

#include <crate/core/decompressor.hh>

namespace crate {

/// XZ streaming decompressor
/// Decompresses xz-compressed data (.xz files)
class CRATE_EXPORT xz_decompressor : public decompressor {
public:
    xz_decompressor();
    ~xz_decompressor() override;

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

/// Raw LZMA streaming decompressor (without xz container)
class CRATE_EXPORT lzma_decompressor : public decompressor {
public:
    lzma_decompressor();
    ~lzma_decompressor() override;

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
