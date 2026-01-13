#include <crate/crate.hh>

#include <crate/formats/ace.hh>
#include <crate/formats/arc.hh>
#include <crate/formats/arj.hh>
#include <crate/formats/cab.hh>
#include <crate/formats/chm.hh>
#include <crate/formats/ha.hh>
#include <crate/formats/hyp.hh>
#include <crate/formats/lha.hh>
#include <crate/formats/rar.hh>
#include <crate/formats/zoo.hh>
#include <optional>
#include <string_view>

namespace crate {
    namespace detail {
        template<typename T>
        result_t <std::unique_ptr <archive>> make_archive(byte_span data) {
            auto result = T::open(data);
            if (!result) return std::unexpected(result.error());
            return std::move(*result);
        }

        // Order matters: more specific formats first, lenient formats last
        // ARC has very lenient signature (single 0x1A byte), so it goes last
        constexpr archive_factory_t builtin_factories[] = {
            make_archive <rar_archive>, // "Rar!" or RAR5 signature
            make_archive <cab_archive>, // "MSCF"
            make_archive <chm_archive>, // "ITSF"
            make_archive <arj_archive>, // 0x60 0xEA
            make_archive <ace_archive>, // "**ACE**"
            make_archive <zoo_archive>, // 0xFDC4A7DC
            make_archive <ha_archive>, // "HA"
            make_archive <hyp_archive>, // 0x1A "HP"
            make_archive <lha_archive>, // "-lh?-" at offset 2
            make_archive <arc_archive>, // 0x1A only - most lenient, try last
        };
    }

    result_t <std::unique_ptr <vfs::archive_vfs>> open_archive(
        byte_span data,
        std::span <const archive_factory_t> user_factories) {
        // Try user factories first
        for (auto factory : user_factories) {
            auto result = factory(data);
            if (result) {
                return vfs::archive_vfs::create(std::move(*result));
            }
        }

        // Try built-in factories
        for (auto factory : detail::builtin_factories) {
            auto result = factory(data);
            if (result) {
                return vfs::archive_vfs::create(std::move(*result));
            }
        }

        return std::unexpected(error(error_code::InvalidSignature,
                                     "Unrecognized archive format"));
    }
} // namespace crate

namespace crate {
    namespace detail {
        bool is_safe_component(std::string_view component) {
            if (component.empty()) return true; // Allow empty (will be skipped)
            if (component == "..") return false;
            if (component == ".") return true; // Current dir is safe
            // Check for null bytes
            if (component.find('\0') != std::string_view::npos) return false;
            return true;
        }

        bool is_safe_relative_path(std::string_view path) {
            if (path.empty()) return true;

            // Reject absolute paths
            if (path[0] == '/' || path[0] == '\\') return false;
#ifdef _WIN32
            // Reject Windows drive letters
            if (path.size() >= 2 && path[1] == ':') return false;
#endif

            // Check for null bytes anywhere in the path
            if (path.find('\0') != std::string_view::npos) return false;

            // Split by both forward and back slashes and check each component
            size_t start = 0;
            size_t depth = 0;

            for (size_t i = 0; i <= path.size(); ++i) {
                if (i == path.size() || path[i] == '/' || path[i] == '\\') {
                    std::string_view component = path.substr(start, i - start);

                    if (component == "..") {
                        if (depth == 0) return false; // Would escape base dir
                        --depth;
                    } else if (!component.empty() && component != ".") {
                        ++depth;
                    }

                    if (!is_safe_component(component)) return false;
                    start = i + 1;
                }
            }

            return true;
        }

        std::optional <std::filesystem::path> safe_join(
            const std::filesystem::path& base,
            std::string_view relative_path) {
            // First validate the relative path before any filesystem operations
            if (!is_safe_relative_path(relative_path)) {
                return std::nullopt;
            }

            // Build the target path
            std::filesystem::path target = base / relative_path;

            // Double-check using canonical paths (defense in depth)
            std::error_code ec;
            auto canonical_base = std::filesystem::weakly_canonical(base, ec);
            if (ec) return std::nullopt;

            auto canonical_target = std::filesystem::weakly_canonical(target, ec);
            if (ec) return std::nullopt;

            // Verify target is under base using path iteration (not string prefix)
            auto base_it = canonical_base.begin();
            auto target_it = canonical_target.begin();

            while (base_it != canonical_base.end()) {
                if (target_it == canonical_target.end()) return std::nullopt;
                if (*base_it != *target_it) return std::nullopt;
                ++base_it;
                ++target_it;
            }

            return target;
        }
    }

    void_result_t dump_vfs(const vfs::archive_vfs& vfs,
                           const std::filesystem::path& dest_dir) {
        // Create destination directory
        std::error_code ec;
        std::filesystem::create_directories(dest_dir, ec);
        if (ec) {
            return std::unexpected(error(error_code::WriteError,
                                         "Failed to create directory: " + dest_dir.string()));
        }

        // Extract all files
        for (const auto& entry : vfs.get_archive()->files()) {
            // Security: validate and join path safely to prevent path traversal
            auto safe_path = detail::safe_join(dest_dir, entry.name);
            if (!safe_path) {
                return std::unexpected(error(error_code::InvalidHeader,
                                             "Unsafe path detected: " + entry.name));
            }

            // Create parent directories
            auto parent = safe_path->parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent, ec);
                if (ec) {
                    return std::unexpected(error(error_code::WriteError,
                                                 "Failed to create directory: " + parent.string()));
                }
            }

            // Extract file
            auto result = vfs.get_archive()->extract(entry, *safe_path);
            if (!result) {
                return result;
            }
        }

        return {};
    }
} // namespace crate
