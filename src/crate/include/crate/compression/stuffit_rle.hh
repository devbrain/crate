#pragma once

#include <crate/core/decompressor.hh>

namespace crate {

// StuffIt RLE90 streaming decompressor
// Uses escape byte 0x90 for run-length encoding:
// - 0x90 0x00 -> literal 0x90
// - 0x90 N    -> repeat previous byte N-1 more times (total N copies)
class CRATE_EXPORT stuffit_rle_decompressor : public decompressor {
public:
    stuffit_rle_decompressor() = default;

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    [[nodiscard]] bool supports_streaming() const override { return true; }

    void reset() override;

private:
    static constexpr byte ESCAPE_BYTE = 0x90;

    // State machine
    enum class state : u8 {
        NORMAL,         // Reading normal bytes
        ESCAPE_PENDING, // Just read 0x90, waiting for count
        REPEATING       // Outputting repeated bytes
    };

    state state_ = state::NORMAL;
    byte last_byte_ = 0;
    unsigned repeat_count_ = 0;  // Remaining repeats
};

}  // namespace crate
