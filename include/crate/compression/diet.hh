#pragma once

#include <crate/core/decompressor.hh>
#include <array>
#include <vector>

namespace crate {

/// DIET decompressor (streaming)
/// DIET was a DOS executable compressor from the early 1990s.
/// It uses LZSS-style compression with variable-length bit encoding.
class CRATE_EXPORT diet_decompressor : public decompressor {
public:
    diet_decompressor();
    ~diet_decompressor() override;

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
