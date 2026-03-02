#pragma once

#include <crate/crate_export.h>

#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>
#include <string>
#include <string_view>
#include <memory>
#include <optional>
#include <limits>
#include <type_traits>

namespace crate {
    // Basic types
    using byte = std::uint8_t;
    using u8 = std::uint8_t;
    using u16 = std::uint16_t;
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;
    using i8 = std::int8_t;
    using i16 = std::int16_t;
    using i32 = std::int32_t;
    using i64 = std::int64_t;

    // Buffer types
    using byte_span = std::span <const byte>;
    using mutable_byte_span = std::span <byte>;
    using byte_vector = std::vector <byte>;

    // CAB compression types
    enum class CompressionType : u16 {
        None = 0x0000,
        MSZIP = 0x0001,
        Quantum = 0x0002,
        LZX = 0x0003
    };

    // Extract LZX window size from compression type
    constexpr u32 lzx_window_bits(u16 comp_type) {
        return (comp_type >> 8) & 0x1F;
    }

    // File attributes
    struct CRATE_EXPORT file_attributes {
        bool readonly: 1 = false;
        bool hidden: 1 = false;
        bool system: 1 = false;
        bool archive: 1 = false;
        bool exec: 1 = false;
        bool name_is_utf8: 1 = false;
    };

    // DOS datetime conversion
    struct CRATE_EXPORT dos_date_time {
        u16 date = 0;
        u16 time = 0;

        [[nodiscard]] constexpr int year() const { return ((date >> 9) & 0x7F) + 1980; }
        [[nodiscard]] constexpr int month() const { return (date >> 5) & 0x0F; }
        [[nodiscard]] constexpr int day() const { return date & 0x1F; }
        [[nodiscard]] constexpr int hour() const { return (time >> 11) & 0x1F; }
        [[nodiscard]] constexpr int minute() const { return (time >> 5) & 0x3F; }
        [[nodiscard]] constexpr int second() const { return (time & 0x1F) * 2; }
    };

    // Little-endian byte reading utilities
    // These are safe alternatives to reinterpret_cast that work on any alignment

    inline constexpr u16 read_u16_le(const u8* p) noexcept {
        return static_cast <u16>(p[0]) | (static_cast <u16>(p[1]) << 8);
    }

    inline constexpr u32 read_u32_le(const u8* p) noexcept {
        return static_cast <u32>(p[0]) |
               (static_cast <u32>(p[1]) << 8) |
               (static_cast <u32>(p[2]) << 16) |
               (static_cast <u32>(p[3]) << 24);
    }

    inline constexpr u64 read_u64_le(const u8* p) noexcept {
        return static_cast <u64>(read_u32_le(p)) |
               (static_cast <u64>(read_u32_le(p + 4)) << 32);
    }

    // Big-endian byte reading utilities

    inline constexpr u16 read_u16_be(const u8* p) noexcept {
        return (static_cast <u16>(p[0]) << 8) | static_cast <u16>(p[1]);
    }

    inline constexpr u32 read_u32_be(const u8* p) noexcept {
        return (static_cast <u32>(p[0]) << 24) |
               (static_cast <u32>(p[1]) << 16) |
               (static_cast <u32>(p[2]) << 8) |
               static_cast <u32>(p[3]);
    }

    inline constexpr u64 read_u64_be(const u8* p) noexcept {
        return (static_cast <u64>(read_u32_be(p)) << 32) |
               static_cast <u64>(read_u32_be(p + 4));
    }

    // Safe arithmetic utilities - detect overflow

    /// Safe addition that returns nullopt on overflow
    template<typename T>
    [[nodiscard]] constexpr std::optional <T> safe_add(T a, T b) noexcept {
        static_assert(std::is_unsigned_v <T>, "safe_add requires unsigned type");
        if (a > std::numeric_limits <T>::max() - b) {
            return std::nullopt;
        }
        return a + b;
    }

    /// Safe multiplication that returns nullopt on overflow
    template<typename T>
    [[nodiscard]] constexpr std::optional <T> safe_mul(T a, T b) noexcept {
        static_assert(std::is_unsigned_v <T>, "safe_mul requires unsigned type");
        if (a == 0 || b == 0) return T{0};
        if (a > std::numeric_limits <T>::max() / b) {
            return std::nullopt;
        }
        return a * b;
    }

    /// Safe cast from larger to smaller unsigned type, returns nullopt if value doesn't fit
    template<typename To, typename From>
    [[nodiscard]] constexpr std::optional <To> checked_cast(From value) noexcept {
        static_assert(std::is_unsigned_v <To> && std::is_unsigned_v <From>,
                      "checked_cast requires unsigned types");
        if constexpr (sizeof(To) >= sizeof(From)) {
            return static_cast <To>(value);
        } else {
            if (value > std::numeric_limits <To>::max()) {
                return std::nullopt;
            }
            return static_cast <To>(value);
        }
    }

    /// Check if buffer has enough bytes remaining
    [[nodiscard]] inline constexpr bool has_bytes(byte_span data, size_t pos, size_t count) noexcept {
        return pos <= data.size() && count <= data.size() - pos;
    }

    /// Check if buffer has enough bytes remaining (size-only version)
    [[nodiscard]] inline constexpr bool has_bytes(size_t data_size, size_t pos, size_t count) noexcept {
        return pos <= data_size && count <= data_size - pos;
    }

    /// Safe substring extraction - returns empty string_view if out of bounds
    [[nodiscard]] inline std::string_view safe_string(byte_span data, size_t pos, size_t max_len) noexcept {
        if (pos >= data.size()) return {};
        size_t available = data.size() - pos;
        size_t len = 0;
        while (len < available && len < max_len && data[pos + len] != 0) {
            ++len;
        }
        return {reinterpret_cast <const char*>(data.data() + pos), len};
    }
} // namespace crate
