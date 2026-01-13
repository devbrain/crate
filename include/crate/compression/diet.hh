#pragma once

#include <crate/core/decompressor.hh>

namespace crate {
    /// DIET decompressor
    /// DIET was a DOS executable compressor from the early 1990s.
    /// It uses LZSS-style compression with variable-length bit encoding.
    class CRATE_EXPORT diet_decompressor : public decompressor {
        public:
            diet_decompressor();
            ~diet_decompressor() override;

            result_t <size_t> decompress(byte_span input, mutable_byte_span output) override;
            void reset() override;

        private:
            struct impl;
            std::unique_ptr <impl> pimpl_;
    };
} // namespace crate
