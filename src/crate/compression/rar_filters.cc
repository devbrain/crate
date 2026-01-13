#include <crate/compression/rar_filters.hh>

namespace crate {

void rar5_filter_processor::add_filter(const rar_filter& filter) {
    filters_.push_back(filter);
}
void rar5_filter_processor::clear() {
    filters_.clear();
}
bool rar5_filter_processor::apply_filters(u8* data, size_t size, u64 file_pos) const {
    bool applied = false;

    for (const auto& filter : filters_) {
        // Check if filter applies to this data range
        if (filter.block_start >= file_pos && filter.block_start + filter.block_length <= file_pos + size) {
            size_t offset = filter.block_start - file_pos;
            size_t length = filter.block_length;

            switch (filter.type) {
                case rar_filter_type::DELTA:
                    apply_delta_filter(data + offset, length, filter.channels);
                    applied = true;
                    break;
                case rar_filter_type::E8:
                    apply_e8_filter(data + offset, length, filter.block_start);
                    applied = true;
                    break;
                case rar_filter_type::E8E9:
                    apply_e8e9_filter(data + offset, length, filter.block_start);
                    applied = true;
                    break;
                case rar_filter_type::ARM:
                    apply_arm_filter(data + offset, length, filter.block_start);
                    applied = true;
                    break;
            }
        }
    }

    return applied;
}
void rar5_filter_processor::apply_delta_filter(u8* data, size_t size, u8 channels) {
    if (channels == 0 || size == 0)
        return;

    // Allocate temporary buffer for reconstruction
    std::vector<u8> temp(size);

    // Source data is stored channel-by-channel
    // We need to interleave and reverse the delta encoding
    size_t src_pos = 0;

    for (u8 ch = 0; ch < channels; ch++) {
        u8 prev_byte = 0;
        size_t dest_pos = ch;

        while (dest_pos < size) {
            // Reverse delta: original = previous - delta
            prev_byte -= data[src_pos++];
            temp[dest_pos] = prev_byte;
            dest_pos += channels;
        }
    }

    std::memcpy(data, temp.data(), size);
}
void rar5_filter_processor::apply_e8_filter(u8* data, size_t size, u64 file_pos) {
    if (size < 5)
        return;

    for (size_t i = 0; i <= size - 5; i++) {
        if (data[i] == 0xE8) {
            // Read absolute address (little-endian)
            i32 addr = static_cast<i32>(data[i + 1]) | (static_cast<i32>(data[i + 2]) << 8) |
                       (static_cast<i32>(data[i + 3]) << 16) | (static_cast<i32>(data[i + 4]) << 24);

            // Convert back to relative
            // During compression: relative -> absolute (addr + file_pos + i + 5)
            // During decompression: absolute -> relative (addr - file_pos - i - 5)
            i64 cur_pos = static_cast<i64>(file_pos) + static_cast<i64>(i) + 5;

            // Only transform if address is within a reasonable range
            // This prevents false positives
            if (addr >= -static_cast<i64>(cur_pos) && addr < (0x01000000 - cur_pos)) {
                addr -= static_cast<i32>(cur_pos);

                // Write back relative address
                data[i + 1] = static_cast<u8>(addr);
                data[i + 2] = static_cast<u8>(addr >> 8);
                data[i + 3] = static_cast<u8>(addr >> 16);
                data[i + 4] = static_cast<u8>(addr >> 24);
            }

            i += 4;  // Skip the address bytes
        }
    }
}
void rar5_filter_processor::apply_e8e9_filter(u8* data, size_t size, u64 file_pos) {
    if (size < 5)
        return;

    for (size_t i = 0; i <= size - 5; i++) {
        if (data[i] == 0xE8 || data[i] == 0xE9) {
            // Read absolute address (little-endian)
            i32 addr = static_cast<i32>(data[i + 1]) | (static_cast<i32>(data[i + 2]) << 8) |
                       (static_cast<i32>(data[i + 3]) << 16) | (static_cast<i32>(data[i + 4]) << 24);

            // Convert back to relative
            i64 cur_pos = static_cast<i64>(file_pos) + static_cast<i64>(i) + 5;

            if (addr >= -static_cast<i64>(cur_pos) && addr < (0x01000000 - cur_pos)) {
                addr -= static_cast<i32>(cur_pos);

                data[i + 1] = static_cast<u8>(addr);
                data[i + 2] = static_cast<u8>(addr >> 8);
                data[i + 3] = static_cast<u8>(addr >> 16);
                data[i + 4] = static_cast<u8>(addr >> 24);
            }

            i += 4;
        }
    }
}
void rar5_filter_processor::apply_arm_filter(u8* data, size_t size, u64 file_pos) {
    if (size < 4)
        return;

    // Process 4-byte aligned instructions
    for (size_t i = 0; i + 3 < size; i += 4) {
        // Check for BL instruction with "always" condition (0xEB)
        if (data[i + 3] == 0xEB) {
            // Read 24-bit offset (little-endian in ARM)
            u32 offset = static_cast<u32>(data[i]) | (static_cast<u32>(data[i + 1]) << 8) |
                         (static_cast<u32>(data[i + 2]) << 16);

            // Convert back to relative
            // ARM BL offset is in words (4 bytes), and PC is 8 bytes ahead
            // During compression: offset was converted to absolute
            // file_pos / 4 gives the word position
            u32 cur_word_pos = static_cast<u32>((file_pos + i) / 4);
            offset -= cur_word_pos;

            // Write back (keep only 24 bits)
            data[i] = static_cast<u8>(offset);
            data[i + 1] = static_cast<u8>(offset >> 8);
            data[i + 2] = static_cast<u8>(offset >> 16);
        }
    }
}
}  // namespace crate
