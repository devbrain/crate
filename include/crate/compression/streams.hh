#pragma once

#include <crate/core/decompressing_stream.hh>
#include <istream>
#include <memory>

namespace crate {

// Always available (built-in inflate/zlib/gzip)
CRATE_EXPORT std::unique_ptr<std::istream> make_inflate_istream(std::istream& source);
CRATE_EXPORT std::unique_ptr<std::istream> make_zlib_istream(std::istream& source);
CRATE_EXPORT std::unique_ptr<std::istream> make_gzip_istream(std::istream& source);

#ifdef CRATE_WITH_BZIP2
CRATE_EXPORT std::unique_ptr<std::istream> make_bzip2_istream(std::istream& source);
#endif

#ifdef CRATE_WITH_XZ
CRATE_EXPORT std::unique_ptr<std::istream> make_xz_istream(std::istream& source);
CRATE_EXPORT std::unique_ptr<std::istream> make_lzma_istream(std::istream& source);
#endif

#ifdef CRATE_WITH_ZSTD
CRATE_EXPORT std::unique_ptr<std::istream> make_zstd_istream(std::istream& source);
#endif

#ifdef CRATE_WITH_BROTLI
CRATE_EXPORT std::unique_ptr<std::istream> make_brotli_istream(std::istream& source);
#endif

} // namespace crate
