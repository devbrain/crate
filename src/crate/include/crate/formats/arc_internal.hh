#pragma once

#include <crate/core/types.hh>

namespace crate::arc {

// RLE90 decompression (DLE = 0x90)
inline byte_vector unpack_rle(byte_span input) {
    byte_vector output;
    output.reserve(input.size() * 2);

    constexpr u8 DLE = 0x90;
    size_t i = 0;

    while (i < input.size()) {
        u8 value = input[i++];
        if (value == DLE) {
            if (i >= input.size()) break;
            u8 count = input[i++];
            if (count == 0) {
                output.push_back(DLE);
            } else {
                if (output.empty()) {
                    for (u8 j = 0; j < count; j++) output.push_back(0);
                } else {
                    u8 last = output.back();
                    for (u8 j = 1; j < count; j++) output.push_back(last);
                }
            }
        } else {
            output.push_back(value);
        }
    }
    return output;
}

} // namespace crate::arc
