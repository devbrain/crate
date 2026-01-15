#pragma once

// FAT directory entry parser with LFN support

#include <crate/core/types.hh>
#include <string>
#include <optional>
#include <vector>

namespace crate::fs {

// Parsed directory entry
struct dir_entry {
    std::string name;           // Full name (LFN or 8.3)
    u32 size;                   // File size in bytes (0 for directories)
    u32 first_cluster;          // Starting cluster (0 for empty files)
    bool is_directory;
    dos_date_time datetime;     // Modification time
    file_attributes attribs;
};

// Directory entry parser
// Reads 32-byte directory entries, handling LFN accumulation
class directory_reader {
public:
    // Construct reader for directory data
    // data: raw directory bytes (must be multiple of 32)
    // max_entries: maximum entries to read (0 = unlimited, use data size)
    explicit directory_reader(byte_span data, u32 max_entries = 0);

    // Read next valid entry
    // Skips deleted entries, volume labels, . and ..
    // Returns nullopt when no more entries
    [[nodiscard]] std::optional<dir_entry> next();

    // Reset to beginning of directory
    void reset();

    // Check if more entries might be available
    [[nodiscard]] bool has_more() const;

private:
    // Internal entry types
    enum class entry_type {
        Empty,          // End of directory
        Deleted,        // Deleted entry (0xE5)
        LongName,       // LFN entry
        VolumeLabel,    // Volume label (skip)
        DotEntry,       // . or .. entry (skip)
        File,           // Regular file
        Directory       // Subdirectory
    };

    // Parse raw 32-byte entry
    entry_type parse_entry(const u8* entry);

    // Convert 8.3 name to string
    static std::string parse_short_name(const u8* name, u8 lowercase_flags);

    // Convert accumulated LFN parts to string
    std::string build_long_name() const;

    byte_span data_;
    u32 max_entries_;
    u32 pos_;                           // Current entry index

    // LFN accumulation
    struct lfn_part {
        u8 sequence;                    // 1-based sequence number
        char16_t chars[13];             // Up to 13 UTF-16 characters
    };
    std::vector<lfn_part> lfn_parts_;

    // Current entry data (for building dir_entry)
    u32 current_size_;
    u32 current_cluster_;
    dos_date_time current_datetime_;
    file_attributes current_attribs_;
    bool current_is_dir_;
    u8 current_name_[11];
    u8 current_lowercase_;
};

// Directory entry attribute bits
namespace dir_attr {
    constexpr u8 READ_ONLY  = 0x01;
    constexpr u8 HIDDEN     = 0x02;
    constexpr u8 SYSTEM     = 0x04;
    constexpr u8 VOLUME_ID  = 0x08;
    constexpr u8 DIRECTORY  = 0x10;
    constexpr u8 ARCHIVE    = 0x20;
    constexpr u8 LONG_NAME  = 0x0F;  // READ_ONLY | HIDDEN | SYSTEM | VOLUME_ID
}

// Special first-byte values
namespace dir_marker {
    constexpr u8 EMPTY      = 0x00;  // Entry never used (end of dir)
    constexpr u8 DELETED    = 0xE5;  // Deleted entry
    constexpr u8 KANJI_E5   = 0x05;  // First char is actually 0xE5 (Kanji)
}

} // namespace crate::fs
