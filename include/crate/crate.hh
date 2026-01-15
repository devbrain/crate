#pragma once

// Core components
#include <crate/core/types.hh>
#include <crate/core/error.hh>
#include <crate/core/system.hh>

// Compression interface
#include <crate/core/decompressor.hh>

// Format extractors
#include <crate/formats/archive.hh>

namespace crate {
    // Version information
    constexpr int VERSION_MAJOR = 1;
    constexpr int VERSION_MINOR = 0;
    constexpr int VERSION_PATCH = 0;

    constexpr const char* VERSION_STRING = "1.0.0";
} // namespace crate
