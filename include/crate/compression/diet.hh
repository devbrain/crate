#pragma once

#include <crate/core/decompressor.hh>

namespace crate {

/// DIET decompressor
/// DIET was a DOS executable compressor from the early 1990s.
/// It uses LZSS-style compression with variable-length bit encoding.
///
/// This decompressor requires all input data in a single call to decompress_some()
/// with input_finished=true. Partial streaming is not supported.
class CRATE_EXPORT diet_decompressor : public decompressor {
public:
    diet_decompressor();
    ~diet_decompressor() override;

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
