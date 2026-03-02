#include <crate/formats/rar.hh>
#include <crate/compression/rar_unpack.hh>
#include <crate/crypto/rar_crypt.hh>
#include <crate/core/crc.hh>
#include <crate/core/path.hh>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace crate {

namespace {
    // Maximum size for a single file extraction to prevent archive bomb attacks
    constexpr size_t MAX_EXTRACTION_SIZE = 1ULL * 1024 * 1024 * 1024; // 1GB
}
    namespace rar {
        struct file_info {
            std::string filename;
            u64 compressed_size = 0; // Total compressed size (all parts)
            u64 original_size = 0;
            u32 crc = 0; // CRC32 of decompressed data (RAR4) or part's compressed data (RAR5)
            dos_date_time datetime{};
            u8 method = 0;
            u8 host_os = 0;
            u32 attribs = 0;
            u64 data_pos = 0; // Position of first part (for backward compat)
            bool is_directory = false;
            bool is_encrypted = false;
            bool continued_from_prev = false;
            bool continued_to_next = false;
            bool solid_file = false; // File depends on previous file's dictionary (solid archive)

            // Multi-volume support
            unsigned start_volume = 0; // Volume where this file starts
            std::vector <file_part> parts; // All parts of this file (for split files)

            // Redirection/link support
            redirection_type redir_type = REDIR_NONE;
            std::string redir_target; // Target filename for hard links/symlinks

            // Encryption support
            std::array <u8, 16> salt{}; // Encryption salt (8 bytes RAR4, 16 bytes RAR5)
            std::array <u8, 16> iv{}; // Initialization vector (RAR5 only)
            u8 kdf_count = 0; // Key derivation iteration count (log2, RAR5 only)
            std::array <u8, 12> pwd_check{}; // Password check value (RAR5, 8 bytes + 4 byte checksum)
            bool has_pwd_check = false; // Whether pwd_check is available
        };

        volume_provider make_file_volume_provider(const std::filesystem::path& first_volume) {
            return [first_volume](unsigned volume_num, const std::string&) -> byte_vector {
                std::filesystem::path vol_path;
                std::string stem = first_volume.stem().string();
                std::string ext = first_volume.extension().string();
                auto parent = first_volume.parent_path();

                // Check for RAR5 naming: name.partN.rar
                if (stem.size() > 6) {
                    size_t part_pos = stem.rfind(".part");
                    if (part_pos != std::string::npos) {
                        // RAR5 style: name.part1.rar, name.part2.rar, ...
                        std::string base = stem.substr(0, part_pos);
                        vol_path = parent / (base + ".part" + std::to_string(volume_num + 1) + ext);
                    }
                }

                if (vol_path.empty()) {
                    // RAR4 style: name.rar, name.r00, name.r01, ...
                    if (volume_num == 0) {
                        vol_path = first_volume;
                    } else {
                        char ext_buf[16];
                        snprintf(ext_buf, sizeof(ext_buf), ".r%02u", volume_num - 1);
                        vol_path = parent / (stem + ext_buf);
                    }
                }

                if (!std::filesystem::exists(vol_path)) {
                    return {};
                }

                std::ifstream file(vol_path, std::ios::binary | std::ios::ate);
                if (!file) return {};

                auto size = file.tellg();
                file.seekg(0);

                if (size <= 0) return {};
                size_t size_value = static_cast <size_t>(size);
                byte_vector data(size_value);
                file.read(reinterpret_cast <char*>(data.data()),
                          static_cast <std::streamsize>(size_value));
                return data;
            };
        }
    }

// PIMPL implementation struct for rar_archive
struct rar_archive::impl {
    std::vector <byte_vector> volumes_;
    unsigned current_volume_ = 0;
    rar::volume_provider volume_provider_;

    rar::version version_ = rar::V4;
    std::vector <rar::file_info> members_;
    std::vector <file_entry> files_;
    size_t sfx_offset_ = 0;
    bool header_encrypted_ = false;
    bool has_split_files_ = false;
    bool all_volumes_loaded_ = false;
    bool is_solid_ = false;
    std::string comment_;

    std::unordered_map <std::string, byte_vector> extracted_cache_;
    std::string password_;

    std::mutex solid_mutex;
    std::unique_ptr<rar_29_decompressor> solid_decomp_v4;
    std::unique_ptr<rar5_decompressor> solid_decomp_v5;
    int solid_last_extracted = -1;
    std::unique_ptr<crypto::rar_decryptor> decryptor;
};

rar_archive::rar_archive() : pimpl_(std::make_unique<impl>()) {}
const byte_vector& rar_archive::data() const {
    return pimpl_->volumes_[pimpl_->current_volume_];
}
void_result_t rar_archive::decrypt_data(const rar::file_info& member, byte_vector& data) const {
    if (!pimpl_->decryptor) {
        pimpl_->decryptor = std::make_unique<crypto::rar_decryptor>();
    }

    if (pimpl_->version_ == rar::V5) {
        // RAR5: AES-256-CBC with PBKDF2 key derivation
        pimpl_->decryptor->init_rar5(pimpl_->password_, member.salt.data(), member.kdf_count, member.iv.data());
    } else {
        // RAR4: AES-128-CBC with SHA-1 based key derivation
        pimpl_->decryptor->init_rar4(pimpl_->password_, member.salt.data());
    }

    // Ensure data is block-aligned (AES block size = 16)
    size_t aligned_size = data.size() & ~size_t(15);
    if (aligned_size == 0) {
        return crate::make_unexpected(error{error_code::CorruptData, "Encrypted data too small"});
    }

    // Decrypt in-place
    size_t decrypted_size = pimpl_->decryptor->decrypt_final(data.data(), aligned_size);

    // Trim to decrypted size (removes padding)
    if (decrypted_size < data.size()) {
        data.resize(decrypted_size);
    }

    return {};
}
void_result_t rar_archive::gather_compressed_data(const rar::file_info& member, byte_vector& out) const {
    // If file has explicit parts, use them
    if (!member.parts.empty()) {
        out.reserve(member.compressed_size);
        for (const auto& part : member.parts) {
            if (part.volume_num >= pimpl_->volumes_.size()) {
                return crate::make_unexpected(
                    error{error_code::TruncatedArchive, "Missing volume " + std::to_string(part.volume_num + 1)});
            }
            const auto& vol = pimpl_->volumes_[part.volume_num];
            size_t vol_size = vol.size();
            if (part.data_pos > vol_size || part.compressed_size > vol_size - static_cast<size_t>(part.data_pos)) {
                return crate::make_unexpected(error{error_code::TruncatedArchive});
            }
            auto start = vol.begin() + static_cast<std::vector<u8>::difference_type>(part.data_pos);
            auto end = start + static_cast<std::vector<u8>::difference_type>(part.compressed_size);
            out.insert(out.end(), start, end);
        }
        return {};
    }

    // Single-part file (backward compat)
    unsigned vol_num = member.start_volume;
    if (vol_num >= pimpl_->volumes_.size()) {
        return crate::make_unexpected(error{error_code::TruncatedArchive, "Missing volume " + std::to_string(vol_num + 1)});
    }
    const auto& vol = pimpl_->volumes_[vol_num];
    size_t vol_size = vol.size();
    if (member.data_pos > vol_size || member.compressed_size > vol_size - static_cast<size_t>(member.data_pos)) {
        return crate::make_unexpected(error{error_code::TruncatedArchive});
    }
    auto start = vol.begin() + static_cast<std::vector<u8>::difference_type>(member.data_pos);
    auto end = start + static_cast<std::vector<u8>::difference_type>(member.compressed_size);
    out.assign(start, end);
    return {};
}
void_result_t rar_archive::decompress_solid(int file_index, byte_span compressed, mutable_byte_span output) const {
    std::lock_guard<std::mutex> lock(pimpl_->solid_mutex);

    // Handle backward seek: if we're extracting a file before our current position,
    // we need to reset the decompressor and start from the beginning
    if (file_index <= pimpl_->solid_last_extracted) {
        // Reset the decompressor state
        pimpl_->solid_decomp_v5.reset();
        pimpl_->solid_decomp_v4.reset();
        pimpl_->solid_last_extracted = -1;
    }

    // Process any missing preceding files to build up the dictionary
    for (int i = pimpl_->solid_last_extracted + 1; i < file_index; i++) {
        const auto& prev_member = pimpl_->members_[static_cast<size_t>(i)];

        // Skip directories and stored files don't affect dictionary
        if (prev_member.is_directory)
            continue;

        bool prev_stored = (pimpl_->version_ == rar::V5) ? (prev_member.method == 0) : (prev_member.method == rar::STORE);
        if (prev_stored)
            continue;

        // Skip encrypted files - we can't process them
        if (prev_member.is_encrypted) {
            return crate::make_unexpected(
                error{error_code::UnsupportedCompression, "Cannot extract solid archive: preceding file is encrypted"});
        }

        // Gather compressed data for preceding file
        byte_vector prev_compressed;
        auto gather_result = gather_compressed_data(prev_member, prev_compressed);
        if (!gather_result) {
            return crate::make_unexpected(gather_result.error());
        }

        // Decompress to build dictionary (output is discarded)
        // Guard against archive bombs
        if (prev_member.original_size > MAX_EXTRACTION_SIZE) {
            return crate::make_unexpected(error{error_code::AllocationLimitExceeded,
                "Previous file in solid archive exceeds maximum allowed size"});
        }
        byte_vector dummy(prev_member.original_size);
        result_t<size_t> result;

        if (pimpl_->version_ == rar::V5) {
            if (!pimpl_->solid_decomp_v5) {
                pimpl_->solid_decomp_v5 = std::make_unique<rar5_decompressor>(false);
                pimpl_->solid_decomp_v5->set_solid_mode(true);
            }
            result = pimpl_->solid_decomp_v5->decompress(byte_span(prev_compressed), dummy);
        } else if (pimpl_->version_ == rar::V4) {
            if (!pimpl_->solid_decomp_v4) {
                pimpl_->solid_decomp_v4 = std::make_unique<rar_29_decompressor>();
                pimpl_->solid_decomp_v4->set_solid_mode(true);
            }
            result = pimpl_->solid_decomp_v4->decompress(byte_span(prev_compressed), dummy);
        }

        if (!result) {
            return crate::make_unexpected(result.error());
        }
    }

    // Now decompress the target file
    result_t<size_t> result;

    if (pimpl_->version_ == rar::V5) {
        if (!pimpl_->solid_decomp_v5) {
            pimpl_->solid_decomp_v5 = std::make_unique<rar5_decompressor>(false);
            pimpl_->solid_decomp_v5->set_solid_mode(true);
        }
        result = pimpl_->solid_decomp_v5->decompress(compressed, output);
    } else if (pimpl_->version_ == rar::V4) {
        if (!pimpl_->solid_decomp_v4) {
            pimpl_->solid_decomp_v4 = std::make_unique<rar_29_decompressor>();
            pimpl_->solid_decomp_v4->set_solid_mode(true);
        }
        result = pimpl_->solid_decomp_v4->decompress(compressed, output);
    }

    if (!result) {
        return crate::make_unexpected(result.error());
    }

    if (*result != output.size()) {
        return crate::make_unexpected(error{error_code::CorruptData, "Decompressed size mismatch"});
    }

    pimpl_->solid_last_extracted = file_index;
    return {};
}
void_result_t rar_archive::parse_remaining_volumes() {
    if (!pimpl_->volume_provider_) {
        // No provider - check if any files need more volumes
        for (const auto& m : pimpl_->members_) {
            if (m.continued_to_next) {
                pimpl_->has_split_files_ = true;
                // Can't load more volumes without provider
                return {};
            }
        }
        pimpl_->all_volumes_loaded_ = true;
        return {};
    }

    // Load volumes until we have all file parts
    while (true) {
        // Check if any file still needs more volumes
        bool need_more = false;
        for (const auto& m : pimpl_->members_) {
            if (m.continued_to_next && !m.parts.empty()) {
                // Check if we have the last part
                auto& last_part = m.parts.back();
                if (last_part.volume_num == pimpl_->volumes_.size() - 1) {
                    // Last part is in current last volume - need to check if continued
                    need_more = true;
                    break;
                }
            }
        }

        if (!need_more) {
            // All files complete or no split files
            break;
        }

        // Try to load next volume
        auto next_vol = pimpl_->volume_provider_(static_cast<unsigned>(pimpl_->volumes_.size()), "");
        if (next_vol.empty()) {
            // No more volumes available
            break;
        }

        pimpl_->volumes_.push_back(std::move(next_vol));

        // Parse the new volume
        pimpl_->current_volume_ = static_cast<unsigned>(pimpl_->volumes_.size() - 1);
        if (pimpl_->version_ == rar::V5) {
            auto result = parse_v5_volume();
            if (!result)
                return result;
        } else {
            auto result = parse_v4_volume();
            if (!result)
                return result;
        }
    }

    // Check if all split files are complete
    pimpl_->all_volumes_loaded_ = true;
    for (const auto& m : pimpl_->members_) {
        if (m.continued_to_next) {
            // Check if file has all parts
            if (m.parts.empty() || (m.parts.back().volume_num < pimpl_->volumes_.size() - 1)) {
                // Still missing volumes
                pimpl_->all_volumes_loaded_ = false;
                break;
            }
        }
    }

    pimpl_->current_volume_ = 0;  // Reset to first volume
    return {};
}
void_result_t rar_archive::detect_and_parse() {
    const auto& vol = pimpl_->volumes_[0];
    if (vol.size() < 8) {
        return crate::make_unexpected(error{error_code::TruncatedArchive});
    }

    // Try signature at offset 0 first (fast path)
    if (std::memcmp(vol.data(), rar::V5_SIGNATURE, 8) == 0) {
        pimpl_->version_ = rar::V5;
        pimpl_->sfx_offset_ = 0;
        return parse_v5();
    }
    if (std::memcmp(vol.data(), rar::V4_SIGNATURE, 7) == 0) {
        pimpl_->version_ = rar::V4;
        pimpl_->sfx_offset_ = 0;
        return parse_v4();
    }
    if (vol.size() >= 4 && std::memcmp(vol.data(), rar::OLD_SIGNATURE, 4) == 0) {
        pimpl_->version_ = rar::OLD;
        pimpl_->sfx_offset_ = 0;
        return parse_old();
    }

    // Scan for RAR signature (SFX support)
    // Limit scan to first 1MB to avoid slow scans on large non-RAR files
    size_t scan_limit = std::min(vol.size(), size_t(1024 * 1024));
    for (size_t i = 1; i < scan_limit - 8; i++) {
        if (std::memcmp(vol.data() + i, rar::V5_SIGNATURE, 8) == 0) {
            pimpl_->version_ = rar::V5;
            pimpl_->sfx_offset_ = i;
            return parse_v5();
        }
        if (std::memcmp(vol.data() + i, rar::V4_SIGNATURE, 7) == 0) {
            pimpl_->version_ = rar::V4;
            pimpl_->sfx_offset_ = i;
            return parse_v4();
        }
    }

    return crate::make_unexpected(error{error_code::InvalidSignature, "Not a valid RAR file"});
}
void_result_t rar_archive::parse_old() {
    const auto& vol = data();
    size_t pos = pimpl_->sfx_offset_ + 4;  // Skip SFX stub + signature

    if (pos + 2 > vol.size()) {
        return crate::make_unexpected(error{error_code::TruncatedArchive});
    }

    u16 hdr_len = read_u16_le(vol.data() + pos);
    pos += 2;

    u8 flags = vol[pos++];
    (void)flags;  // Archive flags

    // Skip archive comment if present
    // ... (simplified for now)

    pos = pimpl_->sfx_offset_ + 4 + hdr_len;

    // Parse members
    while (pos + 12 < vol.size()) {
        rar::file_info member;
        member.start_volume = pimpl_->current_volume_;

        member.compressed_size = read_u32_le(vol.data() + pos);
        pos += 4;
        member.original_size = read_u32_le(vol.data() + pos);
        pos += 4;

        pos += 2;  // checksum

        u16 member_hdr_len = read_u16_le(vol.data() + pos);
        pos += 2;

        if (member_hdr_len < 12 || pos + member_hdr_len > vol.size()) {
            break;
        }

        size_t hdr_start = pos;

        // DOS datetime
        member.datetime.time = read_u16_le(vol.data() + pos);
        member.datetime.date = read_u16_le(vol.data() + pos + 2);
        pos += 4;

        pos += 2;  // attributes

        member.method = rar::STORE + vol[pos + 1];  // Approximate

        u8 fn_len = vol[pos++];
        member.method = vol[pos++];

        if (!has_bytes(vol.size(), pos, fn_len))
            break;
        member.filename = std::string(reinterpret_cast<const char*>(vol.data() + pos), fn_len);
        pos = hdr_start + member_hdr_len;

        member.data_pos = pos;

        // Add file entry
        file_entry entry;
        entry.name = sanitize_path(member.filename);
        entry.uncompressed_size = member.original_size;
        entry.compressed_size = member.compressed_size;
        entry.datetime = member.datetime;
        entry.folder_index = static_cast<u32>(pimpl_->members_.size());
        entry.folder_offset = member.data_pos;

        pimpl_->files_.push_back(entry);
        pimpl_->members_.push_back(member);

        pos += member.compressed_size;
    }

    return {};
}
bool rar_archive::is_valid_utf8(const char* str, size_t len) {
    const u8* s = reinterpret_cast<const u8*>(str);
    size_t i = 0;
    bool has_multibyte = false;
    while (i < len) {
        if (s[i] < 0x80) {
            i++;
        } else if ((s[i] & 0xE0) == 0xC0 && i + 1 < len && (s[i + 1] & 0xC0) == 0x80) {
            has_multibyte = true;
            i += 2;
        } else if ((s[i] & 0xF0) == 0xE0 && i + 2 < len && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80) {
            has_multibyte = true;
            i += 3;
        } else if ((s[i] & 0xF8) == 0xF0 && i + 3 < len && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80 &&
                   (s[i + 3] & 0xC0) == 0x80) {
            has_multibyte = true;
            i += 4;
        } else {
            return false;  // Invalid UTF-8 sequence
        }
    }
    return has_multibyte;  // Only return true if we found actual multibyte chars
}
std::string rar_archive::decode_unicode_filename(const char* name, size_t name_size, const u8* enc_name,
                                                 size_t enc_size) {
    std::u16string wide_name;
    size_t enc_pos = 0, dec_pos = 0;

    u8 high_byte = enc_pos < enc_size ? enc_name[enc_pos++] : 0;
    unsigned flags = 0;
    unsigned flag_bits = 0;

    while (enc_pos < enc_size) {
        if (flag_bits == 0) {
            flags = enc_name[enc_pos++];
            flag_bits = 8;
        }

        switch (flags >> 6) {
            case 0:
                if (enc_pos >= enc_size)
                    break;
                wide_name.push_back(static_cast<char16_t>(enc_name[enc_pos++]));
                dec_pos++;
                break;
            case 1:
                if (enc_pos >= enc_size)
                    break;
                wide_name.push_back(static_cast<char16_t>(enc_name[enc_pos++] | (high_byte << 8)));
                dec_pos++;
                break;
            case 2:
                if (enc_pos + 1 >= enc_size)
                    break;
                wide_name.push_back(static_cast<char16_t>(enc_name[enc_pos] | (enc_name[enc_pos + 1] << 8)));
                enc_pos += 2;
                dec_pos++;
                break;
            case 3: {
                if (enc_pos >= enc_size)
                    break;
                unsigned length = enc_name[enc_pos++];
                if ((length & 0x80) != 0) {
                    if (enc_pos >= enc_size)
                        break;
                    u8 correction = enc_name[enc_pos++];
                    for (length = (length & 0x7f) + 2; length > 0 && dec_pos < name_size; length--, dec_pos++) {
                        u8 ch = static_cast<u8>(name[dec_pos]) + correction;
                        wide_name.push_back(static_cast<char16_t>(ch | (high_byte << 8)));
                    }
                } else {
                    for (length += 2; length > 0 && dec_pos < name_size; length--, dec_pos++) {
                        wide_name.push_back(static_cast<char16_t>(static_cast<u8>(name[dec_pos])));
                    }
                }
                break;
            }
        }

        flags <<= 2;
        flag_bits -= 2;
    }

    // Convert UTF-16 to UTF-8
    std::string result;
    result.reserve(wide_name.size() * 3);
    for (char16_t ch : wide_name) {
        if (ch < 0x80) {
            result.push_back(static_cast<char>(ch));
        } else if (ch < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            result.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xE0 | (ch >> 12)));
            result.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }
    return result;
}
void_result_t rar_archive::parse_v4() {
    return parse_v4_impl(pimpl_->sfx_offset_ + 7, false);
}
void_result_t rar_archive::parse_v4_volume() {
    // Skip signature (7 bytes)
    return parse_v4_impl(7, true);
}
void_result_t rar_archive::parse_v4_impl(size_t start_pos, bool) {
    const auto& vol = data();
    size_t pos = start_pos;

    while (pos + 7 < vol.size()) {
        size_t block_start = pos;

        u16 crc = read_u16_le(vol.data() + pos);
        pos += 2;
        (void)crc;

        u8 type = vol[pos++];
        u16 flags = read_u16_le(vol.data() + pos);
        pos += 2;
        u16 size1 = read_u16_le(vol.data() + pos);
        pos += 2;

        // Ensure we have a valid block size (at least the header we already read)
        if (size1 < 7) {
            return crate::make_unexpected(error{error_code::InvalidHeader, "Invalid RAR block size"});
        }

        u32 size2 = 0;
        if (flags & 0x8000) {
            // Long block
            if (pos + 4 > vol.size())
                break;
            size2 = read_u32_le(vol.data() + pos);
            pos += 4;
        }

        if (type == rar::END_OF_ARCHIVE) {
            break;
        }

        // Parse archive header to detect solid archive
        if (type == rar::ARCHIVE_HEADER) {
            if (flags & rar::MHD_SOLID) {
                pimpl_->is_solid_ = true;
            }
            if (flags & rar::MHD_PASSWORD) {
                pimpl_->header_encrypted_ = true;
            }
            // Skip to next block
            pos = block_start + size1 + size2;
            continue;
        }

        if (type == rar::FILE_HEADER || type == rar::SUBBLOCK_NEW) {
            rar::file_info member;
            member.start_volume = pimpl_->current_volume_;

            if (pos + 25 > vol.size())
                break;

            member.original_size = read_u32_le(vol.data() + pos);
            pos += 4;
            member.host_os = vol[pos++];
            member.crc = read_u32_le(vol.data() + pos);
            pos += 4;

            member.datetime.time = read_u16_le(vol.data() + pos);
            member.datetime.date = read_u16_le(vol.data() + pos + 2);
            pos += 4;

            pos++;  // min version
            member.method = vol[pos++];

            u16 fn_len = read_u16_le(vol.data() + pos);
            pos += 2;
            member.attribs = read_u32_le(vol.data() + pos);
            pos += 4;

            // Handle large files
            u32 size_high = 0;
            if (flags & rar::LHD_LARGE) {
                if (pos + 8 > vol.size())
                    break;
                size_high = read_u32_le(vol.data() + pos);
                pos += 4;
                pos += 4;  // high unpack size
            }

            u64 part_compressed_size = size2 + (static_cast<u64>(size_high) << 32);

            if (!has_bytes(vol.size(), pos, fn_len))
                break;

            // Handle filename encoding
            if (flags & rar::LHD_UNICODE) {
                // Unicode encoded filename: ASCII/UTF-8 name + null + encoded data
                const char* name_ptr = reinterpret_cast<const char*>(vol.data() + pos);
                size_t ascii_len = strnlen(name_ptr, fn_len);

                // Check if the name before null is already valid UTF-8
                // Modern RAR versions may store UTF-8 directly
                if (is_valid_utf8(name_ptr, ascii_len)) {
                    // Already UTF-8, use directly
                    member.filename = std::string(name_ptr, ascii_len);
                } else if (ascii_len < fn_len) {
                    // OEM name with encoded Unicode data - decode it
                    member.filename = decode_unicode_filename(name_ptr, ascii_len, vol.data() + pos + ascii_len + 1,
                                                              fn_len - ascii_len - 1);
                } else {
                    // No Unicode data, use ASCII as-is
                    member.filename = std::string(name_ptr, ascii_len);
                }
            } else {
                // Plain ASCII/OEM filename - truncate at null if present
                const char* name_ptr = reinterpret_cast<const char*>(vol.data() + pos);
                size_t name_len = strnlen(name_ptr, fn_len);
                member.filename = std::string(name_ptr, name_len);
            }
            pos += fn_len;

            // Check flags
            member.continued_from_prev = flags & rar::LHD_SPLIT_BEFORE;
            member.continued_to_next = flags & rar::LHD_SPLIT_AFTER;
            member.is_encrypted = flags & rar::LHD_PASSWORD;
            member.is_directory = ((flags >> 5) & 0x07) == 0x07;
            member.solid_file = (flags & rar::LHD_SOLID) != 0;

            // Read encryption salt if present (RAR4: 8 bytes)
            if ((flags & rar::LHD_SALT) && pos + 8 <= block_start + size1) {
                std::memcpy(member.salt.data(), vol.data() + pos, 8);
                pos += 8;
            }

            if (member.continued_from_prev || member.continued_to_next) {
                pimpl_->has_split_files_ = true;
            }

            // RAR4 Unix symlink detection:
            // Unix symlinks have host_os == OS_UNIX and file mode 0xA000 (symlink)
            // The symlink target is stored as compressed data
            if (member.host_os == rar::OS_UNIX && (member.attribs & 0xF000) == 0xA000) {
                member.redir_type = rar::REDIR_UNIX_SYMLINK;
            }

            // Skip to end of header
            pos = block_start + size1;

            member.data_pos = pos;

            if (type == rar::FILE_HEADER) {
                bool added_to_existing = false;

                // Check if this is a continuation of a previous file
                if (member.continued_from_prev && !pimpl_->members_.empty()) {
                    // Find the file this continues (by name)
                    for (auto& prev : pimpl_->members_) {
                        if (prev.filename == member.filename && prev.continued_to_next) {
                            // Add this part to the previous file
                            rar::file_part part;
                            part.volume_num = pimpl_->current_volume_;
                            part.data_pos = pos;
                            part.compressed_size = part_compressed_size;
                            prev.parts.push_back(part);
                            prev.compressed_size += part_compressed_size;
                            // Update continued_to_next based on this part
                            prev.continued_to_next = member.continued_to_next;
                            added_to_existing = true;
                            break;
                        }
                    }
                }

                if (!added_to_existing) {
                    // Create first part if this is a split file
                    if (member.continued_to_next || member.continued_from_prev) {
                        rar::file_part part;
                        part.volume_num = pimpl_->current_volume_;
                        part.data_pos = pos;
                        part.compressed_size = part_compressed_size;
                        member.parts.push_back(part);
                    }
                    member.compressed_size = part_compressed_size;

                    file_entry entry;
                    entry.name = sanitize_path(member.filename);
                    entry.uncompressed_size = member.original_size;
                    entry.compressed_size = part_compressed_size;
                    entry.datetime = member.datetime;
                    entry.folder_index = static_cast<u32>(pimpl_->members_.size());
                    entry.folder_offset = member.data_pos;

                    // Set attributes
                    if (member.host_os == rar::OS_DOS || member.host_os == rar::OS_OS2 ||
                        member.host_os == rar::OS_WINDOWS) {
                        entry.attribs.readonly = member.attribs & 0x01;
                        entry.attribs.hidden = member.attribs & 0x02;
                        entry.attribs.system = member.attribs & 0x04;
                        entry.attribs.archive = member.attribs & 0x20;
                    }

                    pimpl_->files_.push_back(entry);
                    pimpl_->members_.push_back(member);
                }
            }

            pos += part_compressed_size;
        } else {
            // Skip other block types
            pos = block_start + size1 + size2;
        }
    }

    return {};
}
u64 rar_archive::read_vint(const byte_vector& vol, size_t& pos) {
    u64 value = 0;
    unsigned shift = 0;
    constexpr unsigned MAX_VINT_BYTES = 10;  // 70 bits max, but we only use 64
    unsigned bytes_read = 0;

    while (pos < vol.size()) {
        if (bytes_read >= MAX_VINT_BYTES) {
            // Malformed vint: too many continuation bytes
            return 0;
        }
        u8 b = vol[pos++];
        bytes_read++;

        // Only use the lower bits that fit in u64
        if (shift < 64) {
            value |= static_cast<u64>(b & 0x7F) << shift;
        }

        if ((b & 0x80) == 0) {
            return value;  // End of vint
        }
        shift += 7;
    }
    // Ran out of buffer before finding end marker
    return 0;
}
void_result_t rar_archive::parse_v5() {
    return parse_v5_impl(pimpl_->sfx_offset_ + 8);
}
void_result_t rar_archive::parse_v5_volume() {
    // Skip signature (8 bytes)
    return parse_v5_impl(8);
}
void_result_t rar_archive::parse_v5_impl(size_t start_pos) {
    const auto& vol = data();
    size_t pos = start_pos;

    while (pos + 4 < vol.size()) {
        u32 crc = read_u32_le(vol.data() + pos);
        pos += 4;
        (void)crc;

        u64 hdr_size = read_vint(vol, pos);
        if (hdr_size == 0 || hdr_size > vol.size() - pos)
            break;

        size_t hdr_start = pos;

        auto type = static_cast<unsigned>(read_vint(vol, pos));
        auto hdr_flags = static_cast<unsigned>(read_vint(vol, pos));

        u64 extra_size = 0;
        u64 data_size = 0;

        if (hdr_flags & 0x1) {
            extra_size = read_vint(vol, pos);
        }
        if (hdr_flags & 0x2) {
            data_size = read_vint(vol, pos);
        }

        if (type == rar::V5_EOA) {
            break;
        }

        // Type 1 = Archive header (contains solid flag)
        if (type == rar::V5_ARCHIVE) {
            auto archive_flags = static_cast<unsigned>(read_vint(vol, pos));
            if (archive_flags & rar::AFL_SOLID) {
                pimpl_->is_solid_ = true;
            }
            // Skip to next block
            pos = hdr_start + hdr_size + data_size;
            continue;
        }

        // Type 4 = Encryption header (archive has encrypted headers)
        if (type == rar::V5_ENCRYPTION) {
            pimpl_->header_encrypted_ = true;
            // Skip to next block - we can't parse encrypted headers
            pos = hdr_start + hdr_size + data_size;
            continue;
        }

        if (type == rar::V5_FILE || type == rar::V5_SERVICE) {
            rar::file_info member;
            member.start_volume = pimpl_->current_volume_;

            auto file_flags = static_cast<unsigned>(read_vint(vol, pos));
            member.original_size = read_vint(vol, pos);
            member.attribs = static_cast<u32>(read_vint(vol, pos));

            // mtime and CRC are fixed 4-byte values, NOT vints!
            if (file_flags & 0x2) {
                // Unix timestamp (4 bytes)
                if (pos + 4 > vol.size())
                    break;
                u32 mtime = read_u32_le(vol.data() + pos);
                pos += 4;
                // Convert Unix timestamp to DOS datetime
                // (simplified - just store raw for now)
                (void)mtime;
            }
            if (file_flags & 0x4) {
                // CRC32 (4 bytes)
                // For single-volume RAR5: CRC of unpacked data
                // For multivolume RAR5: CRC of this part's packed data
                if (pos + 4 > vol.size())
                    break;
                member.crc = read_u32_le(vol.data() + pos);
                pos += 4;
            }

            auto cmpr_info = static_cast<unsigned>(read_vint(vol, pos));
            member.method = (cmpr_info >> 7) & 0x07;

            member.host_os = static_cast<u8>(read_vint(vol, pos));

            u64 name_len = read_vint(vol, pos);
            if (name_len > vol.size() || !has_bytes(vol.size(), pos, static_cast<size_t>(name_len)))
                break;
            member.filename =
                std::string(reinterpret_cast<const char*>(vol.data() + pos), static_cast<size_t>(name_len));
            pos += static_cast<size_t>(name_len);

            u64 part_compressed_size = data_size;
            member.is_directory = (file_flags & 0x1) != 0;  // Bit 0 is directory flag

            // Split flags are in header flags, not file flags
            member.continued_from_prev = (hdr_flags & rar::HFL_SPLITBEFORE) != 0;
            member.continued_to_next = (hdr_flags & rar::HFL_SPLITAFTER) != 0;

            if (member.continued_from_prev || member.continued_to_next) {
                pimpl_->has_split_files_ = true;
            }

            // In RAR5 solid archives, all files after the first depend on previous dictionary
            if (pimpl_->is_solid_ && !pimpl_->members_.empty()) {
                member.solid_file = true;
            }

            // Parse extra area for redirection info
            if (extra_size > 0) {
                size_t extra_end = pos + extra_size;
                while (pos < extra_end && pos < vol.size()) {
                    u64 record_size = read_vint(vol, pos);
                    size_t record_end = pos + record_size;
                    if (record_end > extra_end || record_end > vol.size())
                        break;

                    u64 record_type = read_vint(vol, pos);

                    // Type 0x01 = File encryption
                    if (record_type == 0x01) {
                        member.is_encrypted = true;

                        // Parse encryption record
                        // Version (1 byte, should be 0)
                        if (pos < record_end) {
                            u8 enc_version = vol[pos++];
                            (void)enc_version;  // Currently only version 0

                            // Flags (vint)
                            u64 enc_flags = read_vint(vol, pos);

                            // KDF count (1 byte, log2 of iterations)
                            if (pos < record_end) {
                                member.kdf_count = vol[pos++];
                            }

                            // Salt (16 bytes)
                            if (pos + 16 <= record_end) {
                                std::memcpy(member.salt.data(), vol.data() + pos, 16);
                                pos += 16;
                            }

                            // IV (16 bytes)
                            if (pos + 16 <= record_end) {
                                std::memcpy(member.iv.data(), vol.data() + pos, 16);
                                pos += 16;
                            }

                            // Password check value (if flag bit 0 set): 8 + 4 bytes
                            if ((enc_flags & 0x01) && pos + 12 <= record_end) {
                                std::memcpy(member.pwd_check.data(), vol.data() + pos, 12);
                                member.has_pwd_check = true;
                                pos += 12;
                            }
                        }
                    }

                    // Type 0x05 = File system redirection (symlinks, hardlinks)
                    if (record_type == 0x05) {
                        u64 redir_type = read_vint(vol, pos);
                        member.redir_type = static_cast<rar::redirection_type>(redir_type);

                        u64 flags = read_vint(vol, pos);
                        (void)flags;  // Not used for now

                        u64 target_len = read_vint(vol, pos);
                        if (pos + target_len <= record_end) {
                            member.redir_target =
                                std::string(reinterpret_cast<const char*>(vol.data() + pos), target_len);
                        }
                    }

                    pos = record_end;
                }
            }

            // Skip to data (end of header)
            pos = hdr_start + hdr_size;
            member.data_pos = pos;

            // Handle service headers (CMT, QO, etc.)
            if (type == rar::V5_SERVICE) {
                // Check for specific service types
                if (member.filename == rar::SERVICE_CMT) {
                    // Archive comment - decompress if needed
                    size_t data_pos = hdr_start + hdr_size;
                    if (data_size > 0 && data_pos + data_size <= vol.size()) {
                        byte_span compressed(vol.data() + data_pos, data_size);
                        auto result = decompress_service_data(compressed, member.original_size, member.method);
                        if (result) {
                            pimpl_->comment_ = std::string(reinterpret_cast<const char*>(result->data()), result->size());
                        }
                    }
                } else if (member.filename == rar::SERVICE_QO) {
                    // Quick Open record - contains cached file headers
                    // This is optional and mainly for faster archive opening
                    size_t data_pos = hdr_start + hdr_size;
                    if (data_size > 0 && data_pos + data_size <= vol.size()) {
                        byte_span compressed(vol.data() + data_pos, data_size);
                        auto result = decompress_service_data(compressed, member.original_size, member.method);
                        if (result) {
                            // Parse QO data (currently just validates structure)
                            parse_qo_data(byte_span(*result));
                        }
                    }
                }
                // Skip to next block
                pos = hdr_start + hdr_size + data_size;
                continue;
            }

            if (type == rar::V5_FILE) {
                bool added_to_existing = false;

                // Check if this is a continuation of a previous file
                if (member.continued_from_prev && !pimpl_->members_.empty()) {
                    // Find the file this continues (by name)
                    for (auto& prev : pimpl_->members_) {
                        if (prev.filename == member.filename && prev.continued_to_next) {
                            // Add this part to the previous file
                            rar::file_part part;
                            part.volume_num = pimpl_->current_volume_;
                            part.data_pos = pos;
                            part.compressed_size = part_compressed_size;
                            prev.parts.push_back(part);
                            prev.compressed_size += part_compressed_size;
                            // Update continued_to_next based on this part
                            prev.continued_to_next = member.continued_to_next;
                            added_to_existing = true;
                            break;
                        }
                    }
                }

                if (!added_to_existing) {
                    // Create first part if this is a split file
                    if (member.continued_to_next || member.continued_from_prev) {
                        rar::file_part part;
                        part.volume_num = pimpl_->current_volume_;
                        part.data_pos = pos;
                        part.compressed_size = part_compressed_size;
                        member.parts.push_back(part);
                    }
                    member.compressed_size = part_compressed_size;

                    file_entry entry;
                    entry.name = sanitize_path(member.filename);
                    entry.uncompressed_size = member.original_size;
                    entry.compressed_size = part_compressed_size;
                    entry.folder_index = static_cast<u32>(pimpl_->members_.size());
                    entry.folder_offset = member.data_pos;

                    pimpl_->files_.push_back(entry);
                    pimpl_->members_.push_back(member);
                }
            }

            pos += data_size;
        } else {
            // Skip other blocks
            pos = hdr_start + hdr_size + data_size;
        }
    }

    return {};
}
result_t<byte_vector> rar_archive::decompress_service_data(byte_span compressed, u64 unpacked_size, unsigned method) {
    if (method == 0) {
        // Stored - just copy the data
        return byte_vector(compressed.begin(), compressed.end());
    }

    // Create a temporary decompressor for service data
    // Service headers always use non-solid mode
    rar5_decompressor decomp(false);
    byte_vector output(unpacked_size);

    auto result = decomp.decompress(compressed, output);
    if (!result) {
        return crate::make_unexpected(result.error());
    }

    output.resize(*result);
    return output;
}
void_result_t rar_archive::parse_qo_data(byte_span qo_data) {
    size_t pos = 0;

    while (pos + 4 < qo_data.size()) {
        // Each cache structure:
        // - CRC32 (4 bytes)
        // - Size (vint) - size of data starting from Flags
        // - Flags (vint) - currently 0
        // - Offset (vint) - offset from QO header to original header
        // - Data size (vint)
        // - Data (header copy)

        u32 crc = read_u32_le(qo_data.data() + pos);
        pos += 4;
        (void)crc;  // Could verify CRC here

        u64 struct_size = read_vint_from_span(qo_data, pos);
        if (struct_size == 0 || pos + struct_size > qo_data.size())
            break;

        size_t struct_end = pos + struct_size;

        u64 flags = read_vint_from_span(qo_data, pos);
        (void)flags;  // Currently always 0

        u64 offset = read_vint_from_span(qo_data, pos);
        (void)offset;  // Offset to original header (for verification)

        u64 data_size = read_vint_from_span(qo_data, pos);
        if (pos + data_size > struct_end)
            break;

        // The data contains a copy of the header - we could parse it here
        // but since we also parse normal headers, this is mainly for
        // faster archive opening (skip individual header reads)
        // For now, just skip the data
        pos = struct_end;
    }

    return {};
}
u64 rar_archive::read_vint_from_span(byte_span data, size_t& pos) {
    u64 value = 0;
    unsigned shift = 0;
    constexpr unsigned MAX_VINT_BYTES = 10;
    unsigned bytes_read = 0;

    while (pos < data.size()) {
        if (bytes_read >= MAX_VINT_BYTES) {
            return 0;  // Malformed vint
        }
        u8 byte_value = data[pos++];
        bytes_read++;

        if (shift < 64) {
            value |= static_cast<u64>(byte_value & 0x7F) << shift;
        }

        if ((byte_value & 0x80) == 0) {
            return value;
        }
        shift += 7;
    }

    return 0;  // Buffer underflow
}

rar_archive::~rar_archive() = default;
result_t<std::unique_ptr<rar_archive>> rar_archive::open(byte_span data) {
    auto archive = std::unique_ptr<rar_archive>(new rar_archive());
    archive->pimpl_->volumes_.emplace_back(data.begin(), data.end());

    auto result = archive->detect_and_parse();
    if (!result)
        return crate::make_unexpected(result.error());

    return archive;
}
result_t<std::unique_ptr<rar_archive>> rar_archive::open(const std::filesystem::path& path) {
    auto file = file_input_stream::open(path);
    if (!file)
        return crate::make_unexpected(file.error());

    auto size = file->size();
    if (!size)
        return crate::make_unexpected(size.error());

    byte_vector data(*size);
    auto read = file->read(data);
    if (!read)
        return crate::make_unexpected(read.error());

    return open(data);
}
result_t<std::unique_ptr<rar_archive>> rar_archive::open(byte_span first_volume, rar::volume_provider provider) {
    auto archive = std::unique_ptr<rar_archive>(new rar_archive());
    archive->pimpl_->volumes_.emplace_back(first_volume.begin(), first_volume.end());
    archive->pimpl_->volume_provider_ = std::move(provider);

    auto result = archive->detect_and_parse();
    if (!result)
        return crate::make_unexpected(result.error());

    // Parse additional volumes if needed
    auto mv_result = archive->parse_remaining_volumes();
    if (!mv_result)
        return crate::make_unexpected(mv_result.error());

    return archive;
}
result_t<std::unique_ptr<rar_archive>> rar_archive::open_multivolume(const std::filesystem::path& path) {
    auto file = file_input_stream::open(path);
    if (!file)
        return crate::make_unexpected(file.error());

    auto size = file->size();
    if (!size)
        return crate::make_unexpected(size.error());

    byte_vector data(*size);
    auto read = file->read(data);
    if (!read)
        return crate::make_unexpected(read.error());

    return open(data, rar::make_file_volume_provider(path));
}
const std::vector<file_entry>& rar_archive::files() const {
    return pimpl_->files_;
}
result_t<byte_vector> rar_archive::extract(const file_entry& entry) {
    if (entry.folder_index >= pimpl_->members_.size()) {
        return crate::make_unexpected(error{error_code::FileNotInArchive});
    }

    const auto& member = pimpl_->members_[entry.folder_index];

    if (member.is_directory) {
        return byte_vector{};
    }

    if (member.is_encrypted) {
        if (pimpl_->password_.empty()) {
            return crate::make_unexpected(error{error_code::EncryptionError, "Password required for encrypted file"});
        }
    }

    // Handle hard links and file copies
    if (member.redir_type == rar::REDIR_HARDLINK || member.redir_type == rar::REDIR_FILECOPY) {
        // Look up the target file in cache
        auto it = pimpl_->extracted_cache_.find(member.redir_target);
        if (it != pimpl_->extracted_cache_.end()) {
            return it->second;
        }
        // Target not in cache - try to extract it first
        for (size_t i = 0; i < pimpl_->files_.size(); i++) {
            if (pimpl_->members_[i].filename == member.redir_target) {
                auto target_result = extract(pimpl_->files_[i]);
                if (target_result) {
                    // Cache it and return
                    pimpl_->extracted_cache_[member.redir_target] = *target_result;
                    return target_result;
                }
                return target_result;  // Return the error
            }
        }
        return crate::make_unexpected(
            error{error_code::FileNotInArchive, "Hard link target not found: " + member.redir_target});
    }

    // Handle symlinks - return the target path as content
    if (member.redir_type == rar::REDIR_UNIX_SYMLINK || member.redir_type == rar::REDIR_WIN_SYMLINK ||
        member.redir_type == rar::REDIR_WIN_JUNCTION) {
        // RAR5: symlink target is in header metadata (redir_target)
        // RAR4: symlink target is stored as compressed data
        if (!member.redir_target.empty()) {
            // RAR5 style - target is already known
            return byte_vector(member.redir_target.begin(), member.redir_target.end());
        }
        // RAR4 style - target is stored as compressed data, fall through
        // to normal extraction; the decompressed data IS the target path
    }

    // Gather compressed data from all parts (handles multi-volume)
    byte_vector compressed_data;
    auto gather_result = gather_compressed_data(member, compressed_data);
    if (!gather_result) {
        return crate::make_unexpected(gather_result.error());
    }

    // Decrypt if encrypted
    if (member.is_encrypted) {
        auto decrypt_result = decrypt_data(member, compressed_data);
        if (!decrypt_result) {
            return crate::make_unexpected(decrypt_result.error());
        }
    }

    // Guard against archive bombs - reject unreasonably large allocations
    if (member.original_size > MAX_EXTRACTION_SIZE) {
        return crate::make_unexpected(error{error_code::AllocationLimitExceeded,
            "Uncompressed size exceeds maximum allowed (" + std::to_string(member.original_size) + " bytes)"});
    }

    byte_vector output(member.original_size);
    byte_span compressed(compressed_data);

    // Check if file is stored (uncompressed)
    // RAR4: method 0x30 is store
    // RAR5: method 0 is store
    bool is_stored = (pimpl_->version_ == rar::V5) ? (member.method == 0) : (member.method == rar::STORE);

    if (is_stored) {
        // Stored (uncompressed) - data is already gathered
        std::copy_n(compressed_data.data(), member.original_size, output.begin());
        // Update solid tracking even for stored files
        if (pimpl_->is_solid_) {
            std::lock_guard<std::mutex> lock(pimpl_->solid_mutex);
            pimpl_->solid_last_extracted = static_cast<int>(entry.folder_index);
        }
    } else {
        // For solid archives, we need to process files in order
        int file_index = static_cast<int>(entry.folder_index);

        if (pimpl_->is_solid_ && member.solid_file) {
            // This file depends on the previous file's dictionary
            // We need to decompress any missing preceding files first
            auto solid_result = decompress_solid(file_index, compressed, output);
            if (!solid_result) {
                return crate::make_unexpected(solid_result.error());
            }
        } else {
            // Non-solid file or first file in solid archive - use fresh decompressor
            result_t<size_t> result;

            if (pimpl_->version_ == rar::V5) {
                // RAR 5.x uses method 1-5 for compression
                rar5_decompressor decomp(false);
                result = decomp.decompress(compressed, output);
            } else if (pimpl_->version_ == rar::V4) {
                // RAR 4.x uses method 29 (v3.x compression)
                rar_29_decompressor decomp;
                result = decomp.decompress(compressed, output);
            } else {
                // Old RAR format (< v1.50) uses LZ77 + adaptive Huffman
                rar_15_decompressor decomp;
                result = decomp.decompress(compressed, output);
            }

            if (!result) {
                return crate::make_unexpected(result.error());
            }

            if (*result != member.original_size) {
                return crate::make_unexpected(error{error_code::CorruptData, "Decompressed size mismatch"});
            }

            // For solid archives, initialize the shared decompressor from first file
            if (pimpl_->is_solid_ && !member.solid_file) {
                std::lock_guard<std::mutex> lock(pimpl_->solid_mutex);
                // This is the first file - initialize solid decompressor
                if (pimpl_->version_ == rar::V5) {
                    pimpl_->solid_decomp_v5 = std::make_unique<rar5_decompressor>(false);
                    pimpl_->solid_decomp_v5->set_solid_mode(true);
                    // Re-decompress to initialize the dictionary
                    byte_vector dummy(member.original_size);
                    (void)pimpl_->solid_decomp_v5->decompress(compressed, dummy);
                } else if (pimpl_->version_ == rar::V4) {
                    pimpl_->solid_decomp_v4 = std::make_unique<rar_29_decompressor>();
                    pimpl_->solid_decomp_v4->set_solid_mode(true);
                    // Re-decompress to initialize the dictionary
                    byte_vector dummy(member.original_size);
                    (void)pimpl_->solid_decomp_v4->decompress(compressed, dummy);
                }
                pimpl_->solid_last_extracted = file_index;
            }
        }
    }

    // Verify CRC
    // Note: Old RAR format (< v1.50) uses a different 16-bit checksum algorithm
    // that's incompatible with CRC32, so we skip CRC verification for old RAR
    if (pimpl_->version_ != rar::OLD) {
        if (pimpl_->version_ == rar::V5 && !member.parts.empty()) {
            // RAR5 multivolume: CRC is of first part's packed data
            const auto& first_part = member.parts[0];
            if (first_part.volume_num < pimpl_->volumes_.size()) {
                const auto& vol = pimpl_->volumes_[first_part.volume_num];
                if (first_part.data_pos + first_part.compressed_size <= vol.size()) {
                    byte_span first_data(vol.data() + first_part.data_pos, first_part.compressed_size);
                    u32 crc = eval_crc_32(first_data);
                    if (crc != member.crc) {
                        char msg[64];
                        snprintf(msg, sizeof(msg), "CRC mismatch: got %08x expected %08x", crc, member.crc);
                        return crate::make_unexpected(error{error_code::InvalidChecksum, msg});
                    }
                }
            }
        } else {
            // RAR4 or RAR5 single-volume: CRC is of unpacked data
            u32 crc = eval_crc_32(output);
            if (crc != member.crc) {
                char msg[64];
                snprintf(msg, sizeof(msg), "CRC mismatch: got %08x expected %08x", crc, member.crc);
                return crate::make_unexpected(error{error_code::InvalidChecksum, msg});
            }
        }
    }

    // Cache the result for potential hard link references
    pimpl_->extracted_cache_[member.filename] = output;

    // Report byte-level progress
    if (byte_progress_cb_) {
        byte_progress_cb_(entry, output.size(), output.size());
    }

    return output;
}
void_result_t rar_archive::extract(const file_entry& entry, const std::filesystem::path& dest) {
    auto data = extract(entry);
    if (!data)
        return crate::make_unexpected(data.error());

    auto output = file_output_stream::create(dest);
    if (!output)
        return crate::make_unexpected(output.error());

    return output->write(*data);
}
void_result_t rar_archive::extract_all(const std::filesystem::path& dest_dir) {
    for (size_t i = 0; i < pimpl_->files_.size(); i++) {
        const auto& entry = pimpl_->files_[i];

        if (progress_cb_) {
            progress_cb_(entry, i + 1, pimpl_->files_.size());
        }

        // Skip files that can't be extracted
        const auto& member = pimpl_->members_[i];
        if (member.is_encrypted) {
            continue;
        }

        // Skip continuation parts - they're handled when extracting the first part
        if (member.continued_from_prev) {
            continue;
        }

        auto dest = dest_dir / entry.name;
        auto result = extract(entry, dest);
        if (!result)
            return result;
    }
    return {};
}
rar::version rar_archive::version() const {
    return pimpl_->version_;
}
bool rar_archive::is_header_encrypted() const {
    return pimpl_->header_encrypted_;
}
unsigned rar_archive::volume_count() const {
    return static_cast<unsigned>(pimpl_->volumes_.size());
}
bool rar_archive::is_multivolume() const {
    return pimpl_->volumes_.size() > 1 || pimpl_->has_split_files_;
}
bool rar_archive::is_solid() const {
    return pimpl_->is_solid_;
}
const std::string& rar_archive::comment() const {
    return pimpl_->comment_;
}
bool rar_archive::has_comment() const {
    return !pimpl_->comment_.empty();
}
bool rar_archive::has_encrypted_files() const {
    if (pimpl_->header_encrypted_)
        return true;
    for (const auto& m : pimpl_->members_) {
        if (m.is_encrypted)
            return true;
    }
    return false;
}
bool rar_archive::all_extractable() const {
    for (const auto& m : pimpl_->members_) {
        // Encrypted files need a password
        if (m.is_encrypted && pimpl_->password_.empty()) {
            return false;
        }
        // Split files need all volumes loaded
        if (m.continued_to_next && !pimpl_->all_volumes_loaded_) {
            return false;
        }
    }
    return true;
}
void rar_archive::set_password(const std::string& password) {
    pimpl_->password_ = password;
    pimpl_->decryptor.reset();  // Reset decryptor to use new password
}
bool rar_archive::verify_password(const std::string& password) const {
    for (const auto& m : pimpl_->members_) {
        if (m.is_encrypted && m.has_pwd_check) {
            return crypto::rar5_key_derivation::verify_password(password, m.salt.data(), crypto::rar5::SALT_SIZE,
                                                                m.kdf_count, m.pwd_check.data(),
                                                                crypto::rar5::PWD_CHECK_SUM_SIZE);
        }
    }
    return true;  // No check value available
}

} // namespace crate

namespace crate::rar {

} // namespace crate::rar
