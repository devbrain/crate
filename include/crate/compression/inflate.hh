#pragma once

#include <crate/core/decompressor.hh>

namespace crate {

/// Raw DEFLATE streaming decompressor (RFC 1951)
/// Decompresses raw deflate streams without any header/trailer.
class CRATE_EXPORT inflate_decompressor : public decompressor {
public:
    inflate_decompressor();
    ~inflate_decompressor() override;

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

/// Zlib streaming decompressor (RFC 1950)
/// Decompresses zlib-wrapped deflate streams (2-byte header, adler32 checksum).
class CRATE_EXPORT zlib_decompressor : public decompressor {
public:
    zlib_decompressor();
    ~zlib_decompressor() override;

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

/// Gzip streaming decompressor (RFC 1952)
/// Decompresses gzip-wrapped deflate streams (.gz files).
class CRATE_EXPORT gzip_decompressor : public decompressor {
public:
    gzip_decompressor();
    ~gzip_decompressor() override;

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
