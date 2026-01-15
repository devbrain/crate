# Floppy Image Support

## Overview

This document describes read-only extraction support for FAT12/FAT16 floppy disk
images in crate. The implementation treats floppy images as archives, providing
the same `files()` / `extract()` interface as other archive formats.

## Scope

**Supported:**
- FAT12 (360KB, 720KB, 1.2MB, 1.44MB floppies)
- FAT16 (large floppies, small disk images up to ~512MB)
- No partition table (raw filesystem, as typical for floppies)
- 8.3 filenames and VFAT long filenames (LFN)
- Subdirectories (flattened to paths in file listing)
- Read-only extraction

**Not Supported:**
- FAT32 (use ares VFS for that)
- MBR/GPT partition tables
- Write operations
- Bootable disk creation
- Volume labels as entries

## Interface

```cpp
namespace crate {

class floppy_image : public archive {
public:
    /// Open a floppy disk image
    /// @param data Raw disk image bytes (must remain valid for archive lifetime)
    /// @return Archive instance or error
    static result_t<std::unique_ptr<floppy_image>> open(byte_span data);

    /// Get list of all files (directories not included, paths are flattened)
    [[nodiscard]] const std::vector<file_entry>& files() const override;

    /// Extract a file to memory
    [[nodiscard]] result_t<byte_vector> extract(const file_entry& entry) override;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace crate
```

## Usage Example

```cpp
#include <crate/formats/floppy.hh>

// Read floppy image
auto image_data = read_file("dos622.img");
auto floppy = crate::floppy_image::open(image_data).value();

// List files
for (const auto& entry : floppy->files()) {
    std::cout << entry.name << " (" << entry.uncompressed_size << " bytes)\n";
}

// Extract a file
auto content = floppy->extract(floppy->files()[0]).value();
```

## Internal Architecture

```
┌─────────────────────────────────────────────────────┐
│                   floppy_image                       │
│              (public archive interface)              │
└─────────────────────┬───────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────┐
│                   fat_reader                         │
│         (internal FAT filesystem reader)             │
├─────────────────────────────────────────────────────┤
│  - bpb (BIOS Parameter Block)                        │
│  - fat_table (cluster allocation)                    │
│  - directory_reader (entry parsing)                  │
└─────────────────────────────────────────────────────┘
```

### Components

#### 1. BPB Parser (`src/crate/include/crate/fs/fat_bpb.hh`)

Parses the BIOS Parameter Block from sector 0:

```cpp
namespace crate::fs {

enum class fat_type { FAT12, FAT16 };

struct bpb {
    fat_type type;
    u32 bytes_per_sector;      // 512, 1024, 2048, or 4096
    u32 sectors_per_cluster;   // 1, 2, 4, 8, 16, 32, 64, 128
    u32 reserved_sectors;      // Usually 1 for FAT12/16
    u32 num_fats;              // Usually 2
    u32 root_entry_count;      // Max entries in root dir (FAT12/16)
    u32 total_sectors;
    u32 sectors_per_fat;
    u32 cluster_count;         // Determines FAT12 vs FAT16

    // Computed offsets
    u32 fat_offset;            // Byte offset to first FAT
    u32 root_dir_offset;       // Byte offset to root directory
    u32 data_offset;           // Byte offset to cluster 2
    u32 bytes_per_cluster;
};

result_t<bpb> parse_bpb(byte_span data);

} // namespace crate::fs
```

FAT type determination (per Microsoft specification):
- Cluster count < 4085 → FAT12
- Cluster count < 65525 → FAT16
- Otherwise → FAT32 (not supported)

#### 2. FAT Table Reader (`src/crate/include/crate/fs/fat_table.hh`)

Reads cluster chains from the File Allocation Table:

```cpp
namespace crate::fs {

class fat_table {
public:
    fat_table(fat_type type, byte_span fat_data, u32 cluster_count);

    /// Get next cluster in chain (or nullopt if end/bad)
    [[nodiscard]] std::optional<u32> next_cluster(u32 cluster) const;

    /// Check if cluster marks end of chain
    [[nodiscard]] bool is_end_of_chain(u32 cluster) const;

    /// Check if cluster is marked bad
    [[nodiscard]] bool is_bad_cluster(u32 cluster) const;

    /// Get all clusters in a chain starting from first_cluster
    [[nodiscard]] std::vector<u32> get_chain(u32 first_cluster) const;

private:
    fat_type type_;
    byte_span data_;
    u32 cluster_count_;
};

} // namespace crate::fs
```

FAT12 cluster encoding (12-bit packed):
```
Cluster N at offset (N * 3) / 2:
  - If N is even: low 8 bits of byte[0] + low 4 bits of byte[1]
  - If N is odd:  high 4 bits of byte[0] + all 8 bits of byte[1]
```

FAT16 cluster encoding:
```
Cluster N at offset N * 2 (little-endian u16)
```

End-of-chain markers:
- FAT12: 0xFF8 - 0xFFF
- FAT16: 0xFFF8 - 0xFFFF

#### 3. Directory Reader (`src/crate/include/crate/fs/fat_directory.hh`)

Parses 32-byte directory entries:

```cpp
namespace crate::fs {

struct dir_entry {
    std::string name;          // Full name (LFN or 8.3)
    u32 size;                  // File size in bytes
    u32 first_cluster;         // Starting cluster
    bool is_directory;
    dos_date_time datetime;    // Modification time
    file_attributes attribs;
};

class directory_reader {
public:
    directory_reader(byte_span dir_data, u32 max_entries = 0);

    /// Read next entry (skips deleted, volume labels, . and ..)
    [[nodiscard]] std::optional<dir_entry> next();

    /// Reset to beginning
    void reset();

private:
    byte_span data_;
    u32 max_entries_;
    u32 pos_ = 0;
    std::vector<std::u16string> lfn_parts_;  // LFN accumulator
};

} // namespace crate::fs
```

Directory entry format (32 bytes):
```
Offset  Size  Description
0       11    8.3 filename (space-padded)
11      1     Attributes
12      1     Reserved (NT lowercase flags)
13      1     Creation time (tenths of second)
14      2     Creation time
16      2     Creation date
18      2     Last access date
20      2     High word of first cluster (FAT32, zero for FAT12/16)
22      2     Modification time
24      2     Modification date
26      2     Low word of first cluster
28      4     File size
```

LFN entry format (32 bytes, attribute = 0x0F):
```
Offset  Size  Description
0       1     Sequence number (0x40 | N for last, N for others)
1       10    Characters 1-5 (UTF-16LE)
11      1     Attributes (0x0F)
12      1     Type (0)
13      1     Checksum of 8.3 name
14      12    Characters 6-11 (UTF-16LE)
26      2     First cluster (0)
28      4     Characters 12-13 (UTF-16LE)
```

#### 4. FAT Reader (`src/crate/include/crate/fs/fat_reader.hh`)

Combines components for file extraction:

```cpp
namespace crate::fs {

class fat_reader {
public:
    static result_t<fat_reader> open(byte_span image);

    /// Scan all files recursively, building flat path list
    [[nodiscard]] std::vector<file_entry> scan_files() const;

    /// Read file data given first cluster and size
    [[nodiscard]] result_t<byte_vector> read_file(u32 first_cluster, u32 size) const;

    /// Read directory data for a cluster (or root if cluster == 0)
    [[nodiscard]] byte_span read_directory(u32 cluster) const;

private:
    byte_span image_;
    bpb bpb_;
    fat_table fat_;
};

} // namespace crate::fs
```

## Validation

Boot sector validation:
1. Jump instruction: `0xEB ?? 0x90` or `0xE9 ?? ??`
2. Bytes per sector: 512, 1024, 2048, or 4096
3. Sectors per cluster: power of 2, 1-128
4. Media type: 0xF0 or 0xF8-0xFF
5. Root entry count × 32 must be sector-aligned

## Error Handling

New error codes:

```cpp
enum class error_code {
    // ... existing codes ...

    // FAT-specific
    InvalidBootSector,       // Bad jump instruction or signature
    InvalidBpb,              // Invalid BPB values
    UnsupportedFatType,      // FAT32 or exFAT detected
    InvalidCluster,          // Cluster number out of range
    BadClusterChain,         // Corrupted FAT chain
    DirectoryTooDeep,        // Recursion limit exceeded (256 levels)
};
```

## Standard Floppy Geometries

| Format | Capacity | Sectors | Heads | Cylinders | Cluster | FAT Type |
|--------|----------|---------|-------|-----------|---------|----------|
| 5.25" DD | 360 KB | 9 | 2 | 40 | 2 sectors | FAT12 |
| 5.25" HD | 1.2 MB | 15 | 2 | 80 | 1 sector | FAT12 |
| 3.5" DD | 720 KB | 9 | 2 | 80 | 2 sectors | FAT12 |
| 3.5" HD | 1.44 MB | 18 | 2 | 80 | 1 sector | FAT12 |
| 3.5" ED | 2.88 MB | 36 | 2 | 80 | 2 sectors | FAT12 |

Image sizes:
- 360 KB = 368,640 bytes
- 720 KB = 737,280 bytes
- 1.2 MB = 1,228,800 bytes
- 1.44 MB = 1,474,560 bytes
- 2.88 MB = 2,949,120 bytes

## File Organization

```
include/crate/formats/
    floppy.hh              # Public interface

src/crate/include/crate/fs/
    fat_bpb.hh             # BPB structures and parsing
    fat_table.hh           # FAT cluster chain reader
    fat_directory.hh       # Directory entry parser

src/crate/fs/
    fat_bpb.cc
    fat_table.cc
    fat_directory.cc
    floppy.cc              # floppy_image implementation
```

## Testing

Test cases:
1. **Empty floppy**: 1.44MB formatted, no files
2. **Root directory only**: Files in root, no subdirectories
3. **Subdirectories**: Nested directory structure
4. **Long filenames**: VFAT LFN entries
5. **Fragmented files**: Files with non-contiguous clusters
6. **Various sizes**: 360KB, 720KB, 1.2MB, 1.44MB images
7. **Edge cases**: Maximum root entries, deep nesting, deleted entries

Test data location: `testdata/floppy/`

## Relationship with Ares

This implementation is intentionally minimal - read-only extraction only.
For full filesystem access (read/write, mounting, etc.), use ares VFS with
the floppyfs module.

The crate implementation:
- Uses `byte_span` (no seeking, random access via indexing)
- Builds full file list upfront
- Extracts to `byte_vector`
- No write support

The ares implementation:
- Uses `std::istream` (seeking)
- Navigates on-demand
- Supports read/write
- Full VFS integration
