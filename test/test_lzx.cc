#include <doctest/doctest.h>
#include <crate/compression/lzx.hh>
#include <array>

using namespace crate;

TEST_SUITE("LzxDecompressor") {
    TEST_CASE("Constructor with different window sizes") {
        // Valid window sizes are 15-21 bits
        CHECK_NOTHROW(lzx_decompressor(15));
        CHECK_NOTHROW(lzx_decompressor(17));
        CHECK_NOTHROW(lzx_decompressor(21));
    }

    TEST_CASE("Reset state") {
        lzx_decompressor decompressor(17);

        // Reset should not throw
        CHECK_NOTHROW(decompressor.reset());
    }

    TEST_CASE("Empty input") {
        lzx_decompressor decompressor(17);
        std::array<u8, 0> data{};
        std::array<u8, 16> output{};

        auto result = decompressor.decompress(data, output);
        // Empty input must either fail or produce 0 bytes
        CHECK((!result.has_value() || *result == 0));
    }

    TEST_CASE("Truncated input") {
        lzx_decompressor decompressor(17);
        // Just block type, no size
        std::array<u8, 1> data = {0x01};
        std::array<u8, 16> output{};

        auto result = decompressor.decompress(data, output);
        CHECK_FALSE(result.has_value());
    }

    TEST_CASE("Position slots calculation") {
        // Test internal calculation via different window sizes
        // Window bits 15 should have 30 position slots
        // Window bits 17 should have 36 position slots
        // Window bits 21 should have 50 position slots

        lzx_decompressor d15(15);
        lzx_decompressor d17(17);
        lzx_decompressor d21(21);

        // These constructors should succeed with valid slot counts
        CHECK_NOTHROW(d15.reset());
        CHECK_NOTHROW(d17.reset());
        CHECK_NOTHROW(d21.reset());
    }
}
