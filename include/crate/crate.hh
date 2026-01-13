#pragma once

// Core components
#include <crate/core/types.hh>
#include <crate/core/error.hh>
#include <crate/core/system.hh>

// Compression interface
#include <crate/core/decompressor.hh>

// Format extractors
#include <crate/formats/archive.hh>

// VFS layer
#include <crate/core/vfs.hh>

#include <span>
#include <filesystem>

namespace crate {
    // Version information
    constexpr int VERSION_MAJOR = 1;
    constexpr int VERSION_MINOR = 0;
    constexpr int VERSION_PATCH = 0;

    constexpr const char* VERSION_STRING = "1.0.0";

    // Archive factory function type
    using archive_factory_t = result_t <std::unique_ptr <archive>>(*)(byte_span);

    // Open an archive and return VFS interface
    // Tries user-provided factories first, then built-in factories
    CRATE_EXPORT result_t <std::unique_ptr <vfs::archive_vfs>> open_archive(
        byte_span data,
        std::span <const archive_factory_t> user_factories = {});

    // Dump VFS contents to a directory
    CRATE_EXPORT void_result_t dump_vfs(const vfs::archive_vfs& vfs,
                                        const std::filesystem::path& dest_dir);
} // namespace crate
