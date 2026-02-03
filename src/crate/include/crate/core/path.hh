#pragma once

#include <string>
#include <string_view>

namespace crate {

    namespace detail {
        /// Check if position i in path starts an overlong UTF-8 encoding of '/' (0x2F).
        /// These are used by malicious archives to bypass path sanitization:
        /// - C0 AF (2-byte overlong)
        /// - E0 80 AF (3-byte overlong)
        /// - F0 80 80 AF (4-byte overlong)
        /// - F8 80 80 80 AF (5-byte overlong, invalid UTF-8 but may be accepted)
        /// - FC 80 80 80 80 AF (6-byte overlong, invalid UTF-8 but may be accepted)
        inline size_t overlong_slash_length(std::string_view path, size_t i) {
            size_t remaining = path.size() - i;
            auto u = [&](size_t offset) -> unsigned char {
                return static_cast<unsigned char>(path[i + offset]);
            };

            // 2-byte overlong: C0 AF
            if (remaining >= 2 && u(0) == 0xC0 && u(1) == 0xAF) return 2;
            // 3-byte overlong: E0 80 AF
            if (remaining >= 3 && u(0) == 0xE0 && u(1) == 0x80 && u(2) == 0xAF) return 3;
            // 4-byte overlong: F0 80 80 AF
            if (remaining >= 4 && u(0) == 0xF0 && u(1) == 0x80 && u(2) == 0x80 && u(3) == 0xAF) return 4;
            // 5-byte overlong: F8 80 80 80 AF
            if (remaining >= 5 && u(0) == 0xF8 && u(1) == 0x80 && u(2) == 0x80 && u(3) == 0x80 && u(4) == 0xAF) return 5;
            // 6-byte overlong: FC 80 80 80 80 AF
            if (remaining >= 6 && u(0) == 0xFC && u(1) == 0x80 && u(2) == 0x80 && u(3) == 0x80 && u(4) == 0x80 && u(5) == 0xAF) return 6;

            return 0;
        }

        /// Check if a character is a path separator.
        /// Treats forward slash, backslash, and null byte as separators.
        inline bool is_simple_separator(char c) {
            return c == '/' || c == '\\' || c == '\0';
        }
    }

    /// Sanitize a path from an archive to prevent directory traversal attacks.
    /// This function:
    /// - Removes leading slashes (absolute paths become relative)
    /// - Removes ".." path components (prevents escaping extraction directory)
    /// - Converts backslashes to forward slashes (Windows to Unix path separators)
    /// - Removes null bytes and overlong UTF-8 slash encodings (bypass prevention)
    /// - Preserves names that contain ".." as part of the filename (e.g., "data..txt")
    ///
    /// @param path The raw path from the archive
    /// @return Sanitized path safe for extraction
    inline std::string sanitize_path(std::string_view path) {
        std::string result;
        result.reserve(path.size());

        size_t i = 0;
        while (i < path.size()) {
            // Skip leading separators (makes path relative)
            // Check for both simple separators and overlong UTF-8 encodings
            while (i < path.size()) {
                if (detail::is_simple_separator(path[i])) {
                    i++;
                } else if (size_t len = detail::overlong_slash_length(path, i); len > 0) {
                    i += len;
                } else {
                    break;
                }
            }
            if (i >= path.size()) break;

            // Find end of current path component (stopping at any separator)
            size_t component_start = i;
            while (i < path.size()) {
                if (detail::is_simple_separator(path[i])) break;
                if (size_t len = detail::overlong_slash_length(path, i); len > 0) break;
                i++;
            }
            size_t component_end = i;
            std::string_view component = path.substr(component_start, component_end - component_start);

            // Skip ".." components entirely (prevents directory traversal)
            // but preserve names that merely contain ".." (like "data..txt")
            if (component == "..") {
                // Skip any separators after ".."
                while (i < path.size()) {
                    if (detail::is_simple_separator(path[i])) {
                        i++;
                    } else if (size_t len = detail::overlong_slash_length(path, i); len > 0) {
                        i += len;
                    } else {
                        break;
                    }
                }
                continue;
            }

            // Skip "." components (current directory, not useful in archives)
            if (component == ".") {
                while (i < path.size()) {
                    if (detail::is_simple_separator(path[i])) {
                        i++;
                    } else if (size_t len = detail::overlong_slash_length(path, i); len > 0) {
                        i += len;
                    } else {
                        break;
                    }
                }
                continue;
            }

            // Add separator if not first component
            if (!result.empty() && result.back() != '/') {
                result += '/';
            }

            // Add the component (stripping any internal null bytes or backslashes)
            for (char c : component) {
                if (c == '\\') {
                    result += '/';
                } else if (c != '\0') {
                    result += c;
                }
            }
        }

        return result;
    }
} // namespace crate
