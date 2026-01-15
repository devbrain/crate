#include <crate/fs/fat_directory.hh>
#include <algorithm>
#include <cstring>

namespace crate::fs {

// Directory entry structure offsets
namespace entry_off {
    constexpr size_t NAME = 0;          // 11 bytes (8.3 format)
    constexpr size_t ATTR = 11;         // 1 byte
    constexpr size_t NT_RES = 12;       // 1 byte (lowercase flags)
    constexpr size_t CRT_TIME_TENTH = 13;
    constexpr size_t CRT_TIME = 14;     // 2 bytes
    constexpr size_t CRT_DATE = 16;     // 2 bytes
    constexpr size_t LST_ACC_DATE = 18; // 2 bytes
    constexpr size_t FstClusHI = 20;    // 2 bytes (always 0 for FAT12/16)
    constexpr size_t WRT_TIME = 22;     // 2 bytes
    constexpr size_t WRT_DATE = 24;     // 2 bytes
    constexpr size_t FstClusLO = 26;    // 2 bytes
    constexpr size_t FILE_SIZE = 28;    // 4 bytes
}

// LFN entry offsets
namespace lfn_off {
    constexpr size_t ORD = 0;           // 1 byte (sequence)
    constexpr size_t NAME1 = 1;         // 10 bytes (chars 1-5)
    constexpr size_t ATTR = 11;         // 1 byte (always 0x0F)
    constexpr size_t TYPE = 12;         // 1 byte (always 0)
    constexpr size_t CHKSUM = 13;       // 1 byte
    constexpr size_t NAME2 = 14;        // 12 bytes (chars 6-11)
    constexpr size_t FstClusLO = 26;    // 2 bytes (always 0)
    constexpr size_t NAME3 = 28;        // 4 bytes (chars 12-13)
}

constexpr size_t ENTRY_SIZE = 32;

// NT lowercase flags
constexpr u8 LCASE_BASE = 0x08;     // Filename base in lowercase
constexpr u8 LCASE_EXT = 0x10;      // Extension in lowercase

directory_reader::directory_reader(byte_span data, u32 max_entries)
    : data_(data)
    , max_entries_(max_entries)
    , pos_(0)
    , current_size_(0)
    , current_cluster_(0)
    , current_is_dir_(false)
    , current_lowercase_(0) {

    if (max_entries_ == 0) {
        max_entries_ = static_cast<u32>(data_.size() / ENTRY_SIZE);
    }
    std::memset(current_name_, 0, sizeof(current_name_));
}

void directory_reader::reset() {
    pos_ = 0;
    lfn_parts_.clear();
}

bool directory_reader::has_more() const {
    return pos_ < max_entries_ && (pos_ * ENTRY_SIZE) < data_.size();
}

directory_reader::entry_type directory_reader::parse_entry(const u8* entry) {
    u8 first_byte = entry[entry_off::NAME];
    u8 attr = entry[entry_off::ATTR];

    // Check for end of directory
    if (first_byte == dir_marker::EMPTY) {
        return entry_type::Empty;
    }

    // Check for deleted entry
    if (first_byte == dir_marker::DELETED) {
        return entry_type::Deleted;
    }

    // Check for LFN entry
    if ((attr & dir_attr::LONG_NAME) == dir_attr::LONG_NAME) {
        // Parse LFN entry
        lfn_part part{};
        part.sequence = entry[lfn_off::ORD] & 0x1F;

        // Read 13 UTF-16LE characters
        const u8* p1 = entry + lfn_off::NAME1;
        const u8* p2 = entry + lfn_off::NAME2;
        const u8* p3 = entry + lfn_off::NAME3;

        // Characters 1-5 from NAME1
        for (int i = 0; i < 5; i++) {
            part.chars[i] = static_cast<char16_t>(read_u16_le(p1 + i * 2));
        }
        // Characters 6-11 from NAME2
        for (int i = 0; i < 6; i++) {
            part.chars[5 + i] = static_cast<char16_t>(read_u16_le(p2 + i * 2));
        }
        // Characters 12-13 from NAME3
        for (int i = 0; i < 2; i++) {
            part.chars[11 + i] = static_cast<char16_t>(read_u16_le(p3 + i * 2));
        }

        // Check if this is the last (first in sequence) LFN entry
        if (entry[lfn_off::ORD] & 0x40) {
            // Clear any previous LFN parts
            lfn_parts_.clear();
        }

        lfn_parts_.push_back(part);
        return entry_type::LongName;
    }

    // Check for volume label
    if (attr & dir_attr::VOLUME_ID) {
        lfn_parts_.clear();
        return entry_type::VolumeLabel;
    }

    // Check for . or .. entries
    if (first_byte == '.') {
        if (entry[1] == ' ' || (entry[1] == '.' && entry[2] == ' ')) {
            lfn_parts_.clear();
            return entry_type::DotEntry;
        }
    }

    // Regular file or directory - extract data
    std::memcpy(current_name_, entry + entry_off::NAME, 11);
    current_lowercase_ = entry[entry_off::NT_RES];
    current_cluster_ = read_u16_le(entry + entry_off::FstClusLO);
    current_size_ = read_u32_le(entry + entry_off::FILE_SIZE);
    current_datetime_.time = read_u16_le(entry + entry_off::WRT_TIME);
    current_datetime_.date = read_u16_le(entry + entry_off::WRT_DATE);

    // Parse attributes
    current_attribs_ = {};
    current_attribs_.readonly = (attr & dir_attr::READ_ONLY) != 0;
    current_attribs_.hidden = (attr & dir_attr::HIDDEN) != 0;
    current_attribs_.system = (attr & dir_attr::SYSTEM) != 0;
    current_attribs_.archive = (attr & dir_attr::ARCHIVE) != 0;

    current_is_dir_ = (attr & dir_attr::DIRECTORY) != 0;

    return current_is_dir_ ? entry_type::Directory : entry_type::File;
}

std::string directory_reader::parse_short_name(const u8* name, u8 lowercase_flags) {
    std::string base;
    std::string ext;

    // Parse base name (first 8 characters)
    for (int i = 0; i < 8; i++) {
        if (name[i] == ' ') break;
        char c = static_cast<char>(name[i]);
        if (name[i] == dir_marker::KANJI_E5) {
            c = static_cast<char>(0xE5);
        }
        // Apply lowercase if NT flag set
        if ((lowercase_flags & LCASE_BASE) && c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + 32);
        }
        base += c;
    }

    // Parse extension (last 3 characters)
    for (int i = 8; i < 11; i++) {
        if (name[i] == ' ') break;
        char c = static_cast<char>(name[i]);
        if ((lowercase_flags & LCASE_EXT) && c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + 32);
        }
        ext += c;
    }

    if (ext.empty()) {
        return base;
    }
    return base + "." + ext;
}

std::string directory_reader::build_long_name() const {
    if (lfn_parts_.empty()) {
        return {};
    }

    // Sort parts by sequence number (they come in reverse order)
    std::vector<lfn_part> sorted = lfn_parts_;
    std::sort(sorted.begin(), sorted.end(),
              [](const lfn_part& a, const lfn_part& b) {
                  return a.sequence < b.sequence;
              });

    std::string result;
    result.reserve(sorted.size() * 13 * 3);  // Worst case UTF-8

    for (const auto& part : sorted) {
        for (char16_t c : part.chars) {
            if (c == 0 || c == 0xFFFF) {
                // End of name
                return result;
            }

            // Simple UTF-16 to UTF-8 conversion
            if (c < 0x80) {
                result += static_cast<char>(c);
            } else if (c < 0x800) {
                result += static_cast<char>(0xC0 | (c >> 6));
                result += static_cast<char>(0x80 | (c & 0x3F));
            } else {
                result += static_cast<char>(0xE0 | (c >> 12));
                result += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                result += static_cast<char>(0x80 | (c & 0x3F));
            }
        }
    }

    return result;
}

std::optional<dir_entry> directory_reader::next() {
    while (has_more()) {
        size_t offset = static_cast<size_t>(pos_) * ENTRY_SIZE;
        if (offset + ENTRY_SIZE > data_.size()) {
            break;
        }

        const u8* entry_data = data_.data() + offset;
        pos_++;

        entry_type type = parse_entry(entry_data);

        switch (type) {
            case entry_type::Empty:
                // End of directory
                return std::nullopt;

            case entry_type::Deleted:
            case entry_type::VolumeLabel:
            case entry_type::DotEntry:
                // Skip these entries, but clear LFN on non-LFN entries
                continue;

            case entry_type::LongName:
                // LFN part accumulated, continue to next entry
                continue;

            case entry_type::File:
            case entry_type::Directory: {
                dir_entry result;

                // Use LFN if available, otherwise 8.3 name
                std::string lfn = build_long_name();
                if (!lfn.empty()) {
                    result.name = std::move(lfn);
                } else {
                    result.name = parse_short_name(current_name_, current_lowercase_);
                }

                result.size = current_size_;
                result.first_cluster = current_cluster_;
                result.is_directory = current_is_dir_;
                result.datetime = current_datetime_;
                result.attribs = current_attribs_;

                // Clear LFN parts for next entry
                lfn_parts_.clear();

                return result;
            }
        }
    }

    return std::nullopt;
}

} // namespace crate::fs
