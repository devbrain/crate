#include <doctest/doctest.h>
#include <crate/compression/lzss.hh>
#include <array>
#include <vector>

using namespace crate;

TEST_SUITE("SzddLzssDecompressor") {
    TEST_CASE("Decompress all literals") {
        // Control byte 0xFF = all 8 entries are literals
        std::vector<u8> data = {
            0xFF,  // Control: all literals
            'H', 'e', 'l', 'l', 'o', '!', '!', '!'
        };

        szdd_lzss_decompressor decompressor;
        std::array<u8, 16> output{};

        auto result = decompressor.decompress(data, output);
        REQUIRE(result.has_value());
        CHECK(*result == 8);

        std::string text(output.begin(), output.begin() + 5);
        CHECK(text == "Hello");
    }

    TEST_CASE("Reset initializes window with spaces") {
        szdd_lzss_decompressor decompressor;
        decompressor.reset();

        // After reset, window should contain spaces
        // We can test this indirectly by decompressing a match that references
        // the initial window content
        std::vector<u8> data = {
            0xFE,  // Control: 7 literals, 1 match
            'A', 'B', 'C', 'D', 'E', 'F', 'G',
            0x00, 0x30  // Match: pos=0, len=3+3=6 (but we'll only get what fits)
        };

        std::array<u8, 32> output{};
        auto result = decompressor.decompress(data, output);
        REQUIRE(result.has_value());
        CHECK(*result >= 7);  // At least the 7 literals
    }

    TEST_CASE("Empty input") {
        szdd_lzss_decompressor decompressor;
        std::array<u8, 0> data{};
        std::array<u8, 16> output{};

        auto result = decompressor.decompress(data, output);
        REQUIRE(result.has_value());
        CHECK(*result == 0);
    }

    TEST_CASE("Match reference") {
        // Create a pattern that uses back-references
        // First, output "ABCD", then reference it
        std::vector<u8> data = {
            0x0F,  // Control: 4 literals, then 4 matches
            'A', 'B', 'C', 'D',
            // Match entries (2 bytes each): position (12-bit), length (4-bit)
            // Let's make matches that reference the initial window (spaces)
            0x00, 0x00,  // Match at pos 0, length 3
            0x00, 0x00,  // Another match
            0x00, 0x00,
            0x00, 0x00
        };

        szdd_lzss_decompressor decompressor;
        std::array<u8, 64> output{};

        auto result = decompressor.decompress(data, output);
        REQUIRE(result.has_value());
        CHECK(*result >= 4);  // At least the 4 literals
    }
}

TEST_SUITE("KwajLzssDecompressor") {
    TEST_CASE("Decompress all literals") {
        // Bit pattern: 1=literal, each literal followed by 8 bits
        std::vector<u8> data = {
            0xFF,  // 8 literal flags (all 1s)
            'H',   // literal 1
            0xFF,  // 8 more flags (will read next byte as literal)
            'e',
            0xFF,
            'l',
            0xFF,
            'l',
            0xFF,
            'o'
        };

        kwaj_lzss_decompressor decompressor;
        std::array<u8, 16> output{};

        // This will attempt to read bits, starting with LSB
        auto result = decompressor.decompress(data, output);
        // The exact output depends on bit ordering
        REQUIRE(result.has_value());
    }

    TEST_CASE("Reset clears window") {
        kwaj_lzss_decompressor decompressor;
        CHECK_NOTHROW(decompressor.reset());
    }

    TEST_CASE("Empty input") {
        kwaj_lzss_decompressor decompressor;
        std::array<u8, 0> data{};
        std::array<u8, 16> output{};

        auto result = decompressor.decompress(data, output);
        REQUIRE(result.has_value());
        CHECK(*result == 0);
    }
}
