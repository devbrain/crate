#pragma once

// FAT12/FAT16 allocation table reader

#include <crate/core/types.hh>
#include <crate/fs/fat_bpb.hh>
#include <vector>
#include <optional>

namespace crate::fs {

// Cluster status
enum class cluster_status {
    Free,           // Available for allocation
    Used,           // Part of a file chain (value is next cluster)
    Bad,            // Marked as defective
    EndOfChain,     // Last cluster in a file
    Reserved        // Reserved cluster
};

class CRATE_EXPORT fat_table {
public:
    // Construct FAT table reader
    // fat_data: raw FAT table bytes
    // type: FAT12 or FAT16
    // cluster_count: total number of data clusters
    fat_table(byte_span fat_data, fat_type type, u32 cluster_count);

    // Get raw cluster value at index
    [[nodiscard]] u32 get(u32 cluster) const;

    // Get cluster status
    [[nodiscard]] cluster_status status(u32 cluster) const;

    // Check if this is the end of a chain
    [[nodiscard]] bool is_end_of_chain(u32 value) const;

    // Check if cluster is marked bad
    [[nodiscard]] bool is_bad(u32 value) const;

    // Get next cluster in chain, or nullopt if end/bad/free
    [[nodiscard]] std::optional<u32> next(u32 cluster) const;

    // Get entire cluster chain starting from first_cluster
    [[nodiscard]] std::vector<u32> chain(u32 first_cluster) const;

    // Get number of clusters in chain
    [[nodiscard]] u32 chain_length(u32 first_cluster) const;

private:
    byte_span data_;
    fat_type type_;
    u32 cluster_count_;

    // End-of-chain marker thresholds
    u32 eoc_min_;       // Minimum value for end-of-chain
    u32 bad_cluster_;   // Bad cluster marker
};

} // namespace crate::fs
