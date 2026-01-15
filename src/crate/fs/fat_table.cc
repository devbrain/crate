#include <crate/fs/fat_table.hh>

namespace crate::fs {

fat_table::fat_table(byte_span fat_data, fat_type type, u32 cluster_count)
    : data_(fat_data)
    , type_(type)
    , cluster_count_(cluster_count) {

    // Set end-of-chain and bad cluster markers based on FAT type
    if (type_ == fat_type::FAT12) {
        eoc_min_ = 0xFF8;
        bad_cluster_ = 0xFF7;
    } else {
        eoc_min_ = 0xFFF8;
        bad_cluster_ = 0xFFF7;
    }
}

u32 fat_table::get(u32 cluster) const {
    if (cluster >= cluster_count_ + 2) {
        return 0;  // Invalid cluster
    }

    const u8* p = data_.data();

    if (type_ == fat_type::FAT12) {
        // FAT12: 12-bit entries packed into 1.5 bytes each
        // Two entries (cluster N and N+1) occupy 3 bytes starting at offset (N/2)*3
        // Entry N (even): bits 0-11 of the 3 bytes
        // Entry N+1 (odd): bits 12-23 of the 3 bytes
        size_t offset = (cluster / 2) * 3;

        if (offset + 2 >= data_.size()) {
            return 0;
        }

        u8 b0 = p[offset];
        u8 b1 = p[offset + 1];
        u8 b2 = p[offset + 2];

        if (cluster % 2 == 0) {
            // Even cluster: low 8 bits of b0 + low 4 bits of b1
            return static_cast<u32>(b0) | ((static_cast<u32>(b1) & 0x0F) << 8);
        } else {
            // Odd cluster: high 4 bits of b1 + all 8 bits of b2
            return (static_cast<u32>(b1) >> 4) | (static_cast<u32>(b2) << 4);
        }
    } else {
        // FAT16: 16-bit entries, little-endian
        size_t offset = cluster * 2;

        if (offset + 1 >= data_.size()) {
            return 0;
        }

        return read_u16_le(p + offset);
    }
}

cluster_status fat_table::status(u32 cluster) const {
    u32 value = get(cluster);

    if (value == 0) {
        return cluster_status::Free;
    }

    if (value == bad_cluster_) {
        return cluster_status::Bad;
    }

    if (value >= eoc_min_) {
        return cluster_status::EndOfChain;
    }

    if (value == 1 || value >= cluster_count_ + 2) {
        return cluster_status::Reserved;
    }

    return cluster_status::Used;
}

bool fat_table::is_end_of_chain(u32 value) const {
    return value >= eoc_min_;
}

bool fat_table::is_bad(u32 value) const {
    return value == bad_cluster_;
}

std::optional<u32> fat_table::next(u32 cluster) const {
    u32 value = get(cluster);

    if (value == 0 || value == bad_cluster_ || value >= eoc_min_) {
        return std::nullopt;
    }

    if (value < 2 || value >= cluster_count_ + 2) {
        return std::nullopt;  // Invalid cluster reference
    }

    return value;
}

std::vector<u32> fat_table::chain(u32 first_cluster) const {
    std::vector<u32> result;

    if (first_cluster < 2) {
        return result;  // Cluster 0/1 are reserved
    }

    u32 current = first_cluster;
    constexpr u32 MAX_CHAIN = 1000000;  // Sanity limit

    while (result.size() < MAX_CHAIN) {
        result.push_back(current);

        auto n = next(current);
        if (!n) {
            break;
        }

        current = *n;

        // Detect cycles
        if (current == first_cluster) {
            break;
        }
    }

    return result;
}

u32 fat_table::chain_length(u32 first_cluster) const {
    if (first_cluster < 2) {
        return 0;
    }

    u32 count = 0;
    u32 current = first_cluster;
    constexpr u32 MAX_CHAIN = 1000000;

    while (count < MAX_CHAIN) {
        count++;

        auto n = next(current);
        if (!n) {
            break;
        }

        current = *n;

        if (current == first_cluster) {
            break;
        }
    }

    return count;
}

} // namespace crate::fs
