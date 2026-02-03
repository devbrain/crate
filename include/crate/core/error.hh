#pragma once

#include <crate/crate_export.h>

#include <expected>
#include <string>
#include <string_view>

namespace crate {
    enum class error_code {
        Success = 0,

        // I/O errors
        FileNotFound,
        ReadError,
        WriteError,
        SeekError,

        // Format errors
        InvalidSignature,
        InvalidHeader,
        InvalidChecksum,
        UnsupportedVersion,
        UnsupportedCompression,
        CorruptData,

        // Decompression errors
        InvalidHuffmanTable,
        InvalidBlockType,
        InvalidMatchDistance,
        InvalidMatchLength,
        OutputBufferOverflow,
        InputBufferUnderflow,

        // Archive errors
        FileNotInArchive,
        FolderNotFound,
        TruncatedArchive,

        // Parameter errors
        InvalidParameter,

        // Encryption errors
        EncryptionError,
        PasswordRequired,
        InvalidPassword,

        // Resource errors
        AllocationLimitExceeded
    };

    class CRATE_EXPORT error {
        public:
            explicit error(error_code code, std::string message = {})
                : code_(code), message_(std::move(message)) {
            }

            [[nodiscard]] error_code code() const { return code_; }
            [[nodiscard]] std::string_view message() const { return message_; }

            [[nodiscard]] bool is_io_error() const {
                return code_ >= error_code::FileNotFound && code_ <= error_code::SeekError;
            }

            [[nodiscard]] bool is_format_error() const {
                return code_ >= error_code::InvalidSignature && code_ <= error_code::CorruptData;
            }

        private:
            error_code code_;
            std::string message_;
    };

    template<typename T>
    using result_t = std::expected <T, error>;

    using void_result_t = std::expected <void, error>;
} // namespace crate
