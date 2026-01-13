#pragma once

#include <string>
#include <string_view>

namespace crate {
    /// Sanitize a path from an archive to prevent directory traversal attacks.
    /// This function:
    /// - Removes leading slashes (absolute paths become relative)
    /// - Removes ".." path components (prevents escaping extraction directory)
    /// - Converts backslashes to forward slashes (Windows to Unix path separators)
    ///
    /// @param path The raw path from the archive
    /// @return Sanitized path safe for extraction
    inline std::string sanitize_path(std::string_view path) {
        std::string result;
        result.reserve(path.size());

        size_t i = 0;
        while (i < path.size()) {
            // Skip leading slashes (makes path relative)
            if (result.empty() && (path[i] == '/' || path[i] == '\\')) {
                i++;
                continue;
            }

            // Skip ".." sequences entirely (prevents directory traversal)
            // This is aggressive sanitization - removes ".." regardless of context
            if (i + 1 < path.size() && path[i] == '.' && path[i + 1] == '.') {
                i += 2;
                // Also skip any following separator
                if (i < path.size() && (path[i] == '/' || path[i] == '\\')) {
                    i++;
                }
                continue;
            }

            // Convert backslashes to forward slashes
            if (path[i] == '\\') {
                result += '/';
            } else {
                result += path[i];
            }
            i++;
        }

        return result;
    }
} // namespace crate
