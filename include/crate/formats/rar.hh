#pragma once

#include <crate/formats/archive.hh>
#include <functional>
#include <array>
#include <string>
#include <vector>

namespace crate {
    namespace rar {
        struct file_info;

        // Signatures
        constexpr u8 OLD_SIGNATURE[] = {'R', 'E', 0x7E, 0x5E};
        constexpr u8 V4_SIGNATURE[] = {'R', 'a', 'r', '!', 0x1A, 0x07, 0x00};
        constexpr u8 V5_SIGNATURE[] = {'R', 'a', 'r', '!', 0x1A, 0x07, 0x01, 0x00};

        // Volume provider callback type
        // Takes: current volume number (0-based), current volume path (if available)
        // Returns: data for the next volume, or empty vector if no more volumes
        using volume_provider = std::function <byte_vector(unsigned volume_num, const std::string& current_path)>;

        // Simple file-based volume provider that follows RAR naming conventions
        // RAR4: archive.rar, archive.r00, archive.r01, ...
        // RAR5: archive.part1.rar, archive.part2.rar, ...
        CRATE_EXPORT volume_provider make_file_volume_provider(const std::filesystem::path& first_volume);

        // RAR version
        enum version {
            OLD = 1, // < 1.50
            V4 = 4, // 1.50 - 4.20
            V5 = 5 // 5.0+
        };

        // Block types (v4)
        enum block_type : u8 {
            MARKER = 0x72,
            ARCHIVE_HEADER = 0x73,
            FILE_HEADER = 0x74,
            COMMENT = 0x75,
            EXTRA_INFO = 0x76,
            SUBBLOCK_OLD = 0x77,
            RECOVERY_RECORD = 0x78,
            AUTH_INFO = 0x79,
            SUBBLOCK_NEW = 0x7A,
            END_OF_ARCHIVE = 0x7B
        };

        // v5 header types
        enum v5_header_type : unsigned {
            V5_ARCHIVE = 1,
            V5_FILE = 2,
            V5_SERVICE = 3,
            V5_ENCRYPTION = 4,
            V5_EOA = 5
        };

        // Compression methods
        enum method : u8 {
            STORE = 0x30,
            FASTEST = 0x31,
            FAST = 0x32,
            NORMAL = 0x33,
            GOOD = 0x34,
            BEST = 0x35
        };

        // Host OS
        enum host_os : u8 {
            OS_DOS = 0,
            OS_OS2 = 1,
            OS_WINDOWS = 2,
            OS_UNIX = 3,
            OS_MACOS = 4
        };

        // V4 archive header flags (MHD_*)
        constexpr u16 MHD_VOLUME = 0x0001; // Archive is part of multi-volume set
        constexpr u16 MHD_COMMENT = 0x0002; // Archive has comment
        constexpr u16 MHD_LOCK = 0x0004; // Archive is locked
        constexpr u16 MHD_SOLID = 0x0008; // Solid archive (files share dictionary)
        constexpr u16 MHD_NEWNUMBERING = 0x0010; // New volume naming (.partN.rar)
        constexpr u16 MHD_AV = 0x0020; // Authenticity verification
        constexpr u16 MHD_PROTECT = 0x0040; // Recovery record present
        constexpr u16 MHD_PASSWORD = 0x0080; // Block headers are encrypted
        constexpr u16 MHD_FIRSTVOLUME = 0x0100; // First volume of set

        // V4 file header flags (LHD_*)
        constexpr u16 LHD_SPLIT_BEFORE = 0x0001;
        constexpr u16 LHD_SPLIT_AFTER = 0x0002;
        constexpr u16 LHD_PASSWORD = 0x0004;
        constexpr u16 LHD_SOLID = 0x0010; // File uses dictionary from previous file
        constexpr u16 LHD_LARGE = 0x0100;
        constexpr u16 LHD_UNICODE = 0x0200;
        constexpr u16 LHD_SALT = 0x0400;
        constexpr u16 LHD_EXTTIME = 0x1000;

        // V5 archive flags (AFL_*)
        constexpr unsigned AFL_VOLUME = 0x0001; // Archive is volume
        constexpr unsigned AFL_VOLNUMBER = 0x0002; // Volume number field present
        constexpr unsigned AFL_SOLID = 0x0004; // Solid archive

        // V5 header flags (HFL_*)
        constexpr unsigned HFL_EXTRA = 0x0001; // Extra area present
        constexpr unsigned HFL_DATA = 0x0002; // Data area present
        constexpr unsigned HFL_SKIPUNKNOWN = 0x0004; // Skip if unknown type
        constexpr unsigned HFL_SPLITBEFORE = 0x0008; // Data continued from previous volume
        constexpr unsigned HFL_SPLITAFTER = 0x0010; // Data continued in next volume
        constexpr unsigned HFL_DEPENDENT = 0x0020; // Block depends on preceding file block
        constexpr unsigned HFL_KEEPOLD = 0x0040; // Keep old file when saving

        // V5 service header names
        constexpr const char* SERVICE_CMT = "CMT"; // Archive comment
        constexpr const char* SERVICE_QO = "QO"; // Quick open record
        constexpr const char* SERVICE_ACL = "ACL"; // NTFS ACL
        constexpr const char* SERVICE_STM = "STM"; // NTFS stream
        constexpr const char* SERVICE_RR = "RR"; // Recovery record

        // V5 file flags (FFL_*)
        constexpr unsigned FFL_DIRECTORY = 0x0001; // Entry is directory
        constexpr unsigned FFL_UTIME = 0x0002; // Time field present
        constexpr unsigned FFL_CRC32 = 0x0004; // CRC32 field present
        constexpr unsigned FFL_UNPUNKNOWN = 0x0008; // Unpacked size unknown

        // Redirection types for links and references
        enum redirection_type : u8 {
            REDIR_NONE = 0,
            REDIR_UNIX_SYMLINK = 1,
            REDIR_WIN_SYMLINK = 2,
            REDIR_WIN_JUNCTION = 3,
            REDIR_HARDLINK = 4,
            REDIR_FILECOPY = 5
        };

        // Represents one part of a file (possibly spanning volumes)
        struct CRATE_EXPORT file_part {
            unsigned volume_num = 0; // Which volume this part is on
            u64 data_pos = 0; // Position in that volume's data
            u64 compressed_size = 0; // Size of this part
        };

    }

    // RAR archive parser with multi-volume support
    // Supports RAR4 and RAR5 formats, including archives split across multiple volumes.
    class CRATE_EXPORT rar_archive : public archive {
        public:
            using archive::extract;

            // Destructor (required for PIMPL)
            ~rar_archive() override;

            // Open a single-volume archive from memory
            static result_t <std::unique_ptr <rar_archive>> open(byte_span data);

            // Open a single-volume archive from file
            static result_t <std::unique_ptr <rar_archive>> open(const std::filesystem::path& path);

            // Open a single-volume archive from a stream
            static result_t <std::unique_ptr <rar_archive>> open(std::istream& stream);

            // Open a multi-volume archive with custom volume provider
            // The provider is called to get subsequent volumes when needed
            static result_t <std::unique_ptr <rar_archive>> open(byte_span first_volume,
                                                                 rar::volume_provider provider);

            // Open a multi-volume archive from file path (auto-detects volume naming)
            static result_t <std::unique_ptr <rar_archive>> open_multivolume(const std::filesystem::path& path);

            const std::vector <file_entry>& files() const override;

            result_t <byte_vector> extract(const file_entry& entry) override;

            void_result_t extract(const file_entry& entry, const std::filesystem::path& dest) override;

            void_result_t extract_all(const std::filesystem::path& dest_dir) override;

            // Accessors
            [[nodiscard]] rar::version version() const;
            [[nodiscard]] bool is_header_encrypted() const;
            [[nodiscard]] unsigned volume_count() const;
            [[nodiscard]] bool is_multivolume() const;
            [[nodiscard]] bool is_solid() const;
            [[nodiscard]] const std::string& comment() const;
            [[nodiscard]] bool has_comment() const;

            // Check if any file is encrypted
            [[nodiscard]] bool has_encrypted_files() const;

            // Check if decompression is supported for all files
            [[nodiscard]] bool all_extractable() const;

            // Set password for encrypted archives
            void set_password(const std::string& password);

            // Verify password against stored check value (RAR5 only)
            // Returns true if password is likely correct, false otherwise
            [[nodiscard]] bool verify_password(const std::string& password) const;

        private:
            rar_archive();

            // Get current volume data
            const byte_vector& data() const;

            // Decrypt compressed data in-place
            void_result_t decrypt_data(const rar::file_info& member, byte_vector& data) const;

            // Gather all compressed data for a file (handles multi-volume)
            void_result_t gather_compressed_data(const rar::file_info& member, byte_vector& out) const;

            // Decompress a file in a solid archive, processing preceding files as needed
            // Thread-safe: uses mutex to protect shared decompressor state
            void_result_t decompress_solid(int file_index, byte_span compressed, mutable_byte_span output) const;

            // Parse remaining volumes after the first one
            void_result_t parse_remaining_volumes();

            void_result_t detect_and_parse();

            // Parse old RAR format (< v1.50)
            void_result_t parse_old();

            // Check if a string is valid UTF-8
            static bool is_valid_utf8(const char* str, size_t len);

            // Decode RAR v4 Unicode filename encoding
            // The encoded format stores an ASCII name followed by encoded Unicode data
            // Returns UTF-8 string
            static std::string decode_unicode_filename(const char* name, size_t name_size,
                                                       const u8* enc_name, size_t enc_size);

            // Parse RAR v4 format (1.50 - 4.20) - first volume
            void_result_t parse_v4();

            // Parse additional RAR v4 volume
            void_result_t parse_v4_volume();

            // Internal v4 parser
            void_result_t parse_v4_impl(size_t start_pos, bool is_continuation = false);

            // Read RAR5 vint (variable-length integer)
            // Returns 0 on error (caller should check pos advancement)
            // A valid vint can be at most 10 bytes (70 bits, but we only use 64)
            static u64 read_vint(const byte_vector& vol, size_t& pos);

            // Parse RAR v5 format (5.0+) - first volume
            void_result_t parse_v5();

            // Parse additional RAR v5 volume
            void_result_t parse_v5_volume();

            // Internal v5 parser
            void_result_t parse_v5_impl(size_t start_pos);

            // Decompress service header data (used for compressed CMT/QO headers)
            static result_t <byte_vector> decompress_service_data(
                byte_span compressed, u64 unpacked_size, unsigned method);

            // Parse Quick Open (QO) cache structures to extract file headers
            static void_result_t parse_qo_data(byte_span qo_data);

            // Helper to read vint from a ByteSpan
            // Returns 0 on error (malformed vint or buffer underflow)
            static u64 read_vint_from_span(byte_span data, size_t& pos);

            // PIMPL for internal implementation details
            struct impl;
            mutable std::unique_ptr <impl> pimpl_;
    };
} // namespace crate
