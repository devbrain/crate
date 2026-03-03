#pragma once

#include <crate/core/decompressor.hh>
#include <array>
#include <memory>
#include <vector>

namespace crate {

// RNC (Rob Northen Computing) ProPack Decompressor - Streaming implementation
// Supports methods 1 and 2. Method 0 is stored (uncompressed).
class CRATE_EXPORT rnc_decompressor : public decompressor {
public:
    rnc_decompressor();
    ~rnc_decompressor() override;

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

// RNC format constants
namespace rnc {
    constexpr size_t HEADER_SIZE = 18;
    constexpr u32 SIGNATURE = 0x524E43; // "RNC" in big-endian

    // CRC-16 table for RNC format
    CRATE_EXPORT extern const std::array<u16, 256> crc_table;

    // Calculate RNC CRC-16
    CRATE_EXPORT u16 calculate_crc(byte_span data);
}

}  // namespace crate
