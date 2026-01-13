#pragma once

#include <crate/core/types.hh>

namespace crate::hyp {

// HYP checksum: add-then-rotate-left on compressed data
inline u32 hyp_checksum(byte_span data) {
    u32 checksum = 0;
    for (u8 value : data) {
        checksum = (checksum + value);
        // Rotate left 1 bit
        checksum = (checksum << 1) | (checksum >> 31);
    }
    return checksum;
}

} // namespace crate::hyp
