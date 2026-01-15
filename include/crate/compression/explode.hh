#pragma once

#include <crate/core/decompressor.hh>

namespace crate {

/// PKWARE DCL Explode decompressor
/// Decompresses data compressed with the PKWARE Data Compression Library (DCL).
/// This format is sometimes called "implode/explode" and uses LZ77 with
/// predefined Huffman tables. It is used in ZIP files as compression method 10
/// and in various game archives (MPQ, etc).
///
/// Note: This is NOT the same as ZIP's "implode" compression method 6.
///
/// This decompressor requires all input data in a single call to decompress_some()
/// with input_finished=true. Partial streaming is not supported.
class CRATE_EXPORT explode_decompressor : public decompressor {
public:
    explode_decompressor();
    ~explode_decompressor() override;

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
