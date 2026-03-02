#include <crate/compression/streams.hh>
#include <crate/compression/inflate.hh>

#ifdef CRATE_WITH_BZIP2
#include <crate/compression/bzip2.hh>
#endif

#ifdef CRATE_WITH_XZ
#include <crate/compression/xz.hh>
#endif

#ifdef CRATE_WITH_ZSTD
#include <crate/compression/zstd.hh>
#endif

#ifdef CRATE_WITH_BROTLI
#include <crate/compression/brotli.hh>
#endif

namespace crate {

std::unique_ptr<std::istream> make_inflate_istream(std::istream& source) {
    return std::make_unique<idecompressing_stream>(
        source, std::make_unique<inflate_decompressor>());
}

std::unique_ptr<std::istream> make_zlib_istream(std::istream& source) {
    return std::make_unique<idecompressing_stream>(
        source, std::make_unique<zlib_decompressor>());
}

std::unique_ptr<std::istream> make_gzip_istream(std::istream& source) {
    return std::make_unique<idecompressing_stream>(
        source, std::make_unique<gzip_decompressor>());
}

#ifdef CRATE_WITH_BZIP2
std::unique_ptr<std::istream> make_bzip2_istream(std::istream& source) {
    return std::make_unique<idecompressing_stream>(
        source, std::make_unique<bzip2_decompressor>());
}
#endif

#ifdef CRATE_WITH_XZ
std::unique_ptr<std::istream> make_xz_istream(std::istream& source) {
    return std::make_unique<idecompressing_stream>(
        source, std::make_unique<xz_decompressor>());
}

std::unique_ptr<std::istream> make_lzma_istream(std::istream& source) {
    return std::make_unique<idecompressing_stream>(
        source, std::make_unique<lzma_decompressor>());
}
#endif

#ifdef CRATE_WITH_ZSTD
std::unique_ptr<std::istream> make_zstd_istream(std::istream& source) {
    return std::make_unique<idecompressing_stream>(
        source, std::make_unique<zstd_decompressor>());
}
#endif

#ifdef CRATE_WITH_BROTLI
std::unique_ptr<std::istream> make_brotli_istream(std::istream& source) {
    return std::make_unique<idecompressing_stream>(
        source, std::make_unique<brotli_decompressor>());
}
#endif

} // namespace crate
