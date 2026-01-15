#include <crate/fs/fat_bpb.hh>

namespace crate::fs {

// BPB structure offsets (from start of boot sector)
namespace offsets {
    constexpr size_t JUMP_BOOT = 0;         // 3 bytes
    constexpr size_t OEM_NAME = 3;          // 8 bytes
    constexpr size_t BYTES_PER_SEC = 11;    // 2 bytes
    constexpr size_t SEC_PER_CLUS = 13;     // 1 byte
    constexpr size_t RSVD_SEC_CNT = 14;     // 2 bytes
    constexpr size_t NUM_FATS = 16;         // 1 byte
    constexpr size_t ROOT_ENT_CNT = 17;     // 2 bytes
    constexpr size_t TOT_SEC_16 = 19;       // 2 bytes
    constexpr size_t MEDIA = 21;            // 1 byte
    constexpr size_t FAT_SZ_16 = 22;        // 2 bytes
    constexpr size_t SEC_PER_TRK = 24;      // 2 bytes
    constexpr size_t NUM_HEADS = 26;        // 2 bytes
    constexpr size_t HIDD_SEC = 28;         // 4 bytes
    constexpr size_t TOT_SEC_32 = 32;       // 4 bytes
}

result_t<bpb> parse_bpb(byte_span data) {
    if (data.size() < 512) {
        return std::unexpected(error{error_code::InvalidHeader, "Boot sector too small"});
    }

    const u8* p = data.data();

    // Validate jump instruction
    // Must be 0xEB ?? 0x90 (short jump + NOP) or 0xE9 ?? ?? (near jump)
    if (p[offsets::JUMP_BOOT] == 0xEB) {
        if (p[offsets::JUMP_BOOT + 2] != 0x90) {
            // Some images have different third byte, just warn mentally
        }
    } else if (p[offsets::JUMP_BOOT] != 0xE9) {
        return std::unexpected(error{error_code::InvalidHeader, "Invalid boot sector jump instruction"});
    }

    bpb result{};

    // Read BPB fields
    result.bytes_per_sector = read_u16_le(p + offsets::BYTES_PER_SEC);
    result.sectors_per_cluster = p[offsets::SEC_PER_CLUS];
    result.reserved_sectors = read_u16_le(p + offsets::RSVD_SEC_CNT);
    result.num_fats = p[offsets::NUM_FATS];
    result.root_entry_count = read_u16_le(p + offsets::ROOT_ENT_CNT);

    u16 tot_sec_16 = read_u16_le(p + offsets::TOT_SEC_16);
    u32 tot_sec_32 = read_u32_le(p + offsets::TOT_SEC_32);
    result.total_sectors = (tot_sec_16 != 0) ? tot_sec_16 : tot_sec_32;

    u8 media = p[offsets::MEDIA];
    result.sectors_per_fat = read_u16_le(p + offsets::FAT_SZ_16);

    // Validate bytes per sector
    switch (result.bytes_per_sector) {
        case 512:
        case 1024:
        case 2048:
        case 4096:
            break;
        default:
            return std::unexpected(error{error_code::InvalidHeader,
                "Invalid bytes per sector: " + std::to_string(result.bytes_per_sector)});
    }

    // Validate sectors per cluster (must be power of 2)
    switch (result.sectors_per_cluster) {
        case 1: case 2: case 4: case 8:
        case 16: case 32: case 64: case 128:
            break;
        default:
            return std::unexpected(error{error_code::InvalidHeader,
                "Invalid sectors per cluster: " + std::to_string(result.sectors_per_cluster)});
    }

    // Validate media type (0xF0 or 0xF8-0xFF)
    if (media != 0xF0 && media < 0xF8) {
        return std::unexpected(error{error_code::InvalidHeader,
            "Invalid media type: " + std::to_string(media)});
    }

    // Validate root entry count alignment
    u32 root_dir_bytes = result.root_entry_count * 32;
    if (root_dir_bytes % result.bytes_per_sector != 0) {
        return std::unexpected(error{error_code::InvalidHeader,
            "Root entry count not sector-aligned"});
    }

    // Validate we have sectors
    if (result.total_sectors == 0) {
        return std::unexpected(error{error_code::InvalidHeader, "Total sectors is zero"});
    }

    // Calculate derived values
    u32 root_dir_sectors = (result.root_entry_count * 32 + result.bytes_per_sector - 1)
                           / result.bytes_per_sector;

    u32 data_sectors = result.total_sectors
                       - result.reserved_sectors
                       - (result.num_fats * result.sectors_per_fat)
                       - root_dir_sectors;

    result.cluster_count = data_sectors / result.sectors_per_cluster;

    // Determine FAT type based on cluster count (Microsoft specification)
    if (result.cluster_count < 4085) {
        result.type = fat_type::FAT12;
    } else if (result.cluster_count < 65525) {
        result.type = fat_type::FAT16;
    } else {
        return std::unexpected(error{error_code::UnsupportedCompression,
            "FAT32 is not supported"});
    }

    // Calculate byte offsets
    result.fat_offset = result.reserved_sectors * result.bytes_per_sector;
    result.root_dir_offset = result.fat_offset +
                             (result.num_fats * result.sectors_per_fat * result.bytes_per_sector);
    result.root_dir_size = root_dir_sectors * result.bytes_per_sector;
    result.data_offset = result.root_dir_offset + result.root_dir_size;
    result.bytes_per_cluster = result.sectors_per_cluster * result.bytes_per_sector;

    return result;
}

} // namespace crate::fs
