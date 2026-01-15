#pragma once

// FAT12/FAT16 BIOS Parameter Block parsing

#include <crate/core/types.hh>
#include <crate/core/error.hh>

namespace crate::fs {

enum class fat_type : u8 {
    FAT12,
    FAT16
};

// Parsed BIOS Parameter Block
struct bpb {
    fat_type type;
    u32 bytes_per_sector;       // 512, 1024, 2048, or 4096
    u32 sectors_per_cluster;    // 1, 2, 4, 8, 16, 32, 64, 128
    u32 reserved_sectors;       // Usually 1 for FAT12/16
    u32 num_fats;               // Usually 2
    u32 root_entry_count;       // Max entries in root dir (FAT12/16)
    u32 total_sectors;
    u32 sectors_per_fat;
    u32 cluster_count;          // Determines FAT12 vs FAT16

    // Computed offsets (byte positions)
    u32 fat_offset;             // Byte offset to first FAT
    u32 root_dir_offset;        // Byte offset to root directory
    u32 root_dir_size;          // Size of root directory in bytes
    u32 data_offset;            // Byte offset to cluster 2 (first data cluster)
    u32 bytes_per_cluster;
};

// Parse BPB from boot sector
// Expects at least 512 bytes of data
result_t<bpb> parse_bpb(byte_span data);

// Get byte offset for a given cluster number
// Cluster numbering starts at 2 (0 and 1 are reserved)
inline u32 cluster_offset(const bpb& b, u32 cluster) {
    if (cluster < 2) {
        return b.root_dir_offset;  // Cluster 0 means root directory
    }
    return b.data_offset + (cluster - 2) * b.bytes_per_cluster;
}

} // namespace crate::fs
