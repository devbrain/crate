#pragma once

#include <array>
#include <crate/core/decompressor.hh>
#include <crate/core/bitstream.hh>
#include <crate/core/types.hh>

namespace crate {
// ARJ Method 4 LZ77 decompressor
// This is a custom LZ77 algorithm with variable-length encoding for lengths and offsets
class CRATE_EXPORT arj_method4_decompressor : public decompressor {
public:
    static constexpr size_t WINDOW_SIZE = 16384;  // 16KB window
    static constexpr size_t WINDOW_MASK = WINDOW_SIZE - 1;

    arj_method4_decompressor(bool old_format = false);

    result_t<size_t> decompress(byte_span input, mutable_byte_span output) override;

    void reset() override;

private:
    void init_state();
    // Read length code using unary-like encoding
    // Returns 0 for literal, or (length - 2) for match
    result_t<unsigned> read_length_code(msb_bitstream& bs) const;

    // Read offset using variable-length encoding
    // Maximum offset is 15871 (fits in 16KB window)
    result_t<unsigned> read_offset(msb_bitstream& bs) const;

    std::array<u8, WINDOW_SIZE> window_{};
    u32 window_pos_ = 0;
    bool old_format_ = false;
};
}  // namespace crate
