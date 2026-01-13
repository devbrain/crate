#include <doctest/doctest.h>
#include <crate/formats/szdd.hh>
#include <crate/test_config.hh>
#include <array>
#include <vector>

using namespace crate;

TEST_SUITE("SzddExtractor") {
    TEST_CASE("Parse SZDD header - standard") {
        std::array<u8, 14> data = {
            'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33,  // Signature
            'A',  // Compression method
            'E',  // Missing character
            0x00, 0x10, 0x00, 0x00  // Uncompressed size (4096)
        };

        auto header = szdd_extractor::parse_header(data);
        REQUIRE(header.has_value());
        CHECK(header->comp_method == 'A');
        CHECK(header->missing_char == 'E');
        CHECK(header->uncompressed_size == 4096);
        CHECK_FALSE(header->is_qbasic);
    }

    TEST_CASE("Parse SZDD header - QBasic variant") {
        std::array<u8, 14> data = {
            'S', 'Z', ' ', 0x88, 0xF0, 0x27, 0x33,  // QBasic signature (7 bytes)
            0x00, 0x10, 0x00, 0x00,  // Uncompressed size (4096)
            0, 0, 0  // Padding
        };

        auto header = szdd_extractor::parse_header(data);
        REQUIRE(header.has_value());
        CHECK(header->comp_method == 'A');
        CHECK(header->missing_char == '\0');
        CHECK(header->uncompressed_size == 4096);
        CHECK(header->is_qbasic);
    }

    TEST_CASE("Invalid signature") {
        std::array<u8, 14> data = {
            'N', 'O', 'P', 'E', 0x88, 0xF0, 0x27, 0x33,
            'A', 'E', 0x00, 0x10, 0x00, 0x00
        };

        auto header = szdd_extractor::parse_header(data);
        CHECK_FALSE(header.has_value());
        CHECK(header.error().code() == error_code::InvalidSignature);
    }

    TEST_CASE("Unsupported compression method") {
        std::array<u8, 14> data = {
            'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33,
            'B',  // Unsupported method (not 'A')
            'E',
            0x00, 0x10, 0x00, 0x00
        };

        auto header = szdd_extractor::parse_header(data);
        CHECK_FALSE(header.has_value());
        CHECK(header.error().code() == error_code::UnsupportedCompression);
    }

    TEST_CASE("Recover filename") {
        CHECK(szdd_extractor::recover_filename("SETUP.EX_", 'E') == "SETUP.EXE");
        CHECK(szdd_extractor::recover_filename("README.TX_", 'T') == "README.TXT");
        CHECK(szdd_extractor::recover_filename("FOO.DL_", 'L') == "FOO.DLL");
        CHECK(szdd_extractor::recover_filename("BAR.SY_", 'S') == "BAR.SYS");
    }

    TEST_CASE("Recover filename - no underscore") {
        CHECK(szdd_extractor::recover_filename("NOUNDER.TXT", 'X') == "NOUNDER.TXT");
    }

    TEST_CASE("Recover filename - null missing char") {
        CHECK(szdd_extractor::recover_filename("FILE.EX_", '\0') == "FILE.EX_");
    }

    TEST_CASE("Decompress simple SZDD data - all literals") {
        // Create SZDD with simple compressed content
        // Control byte 0xFF = all 8 bits are literals
        std::vector<u8> data = {
            'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33,
            'A', '_',
            0x08, 0x00, 0x00, 0x00,  // Uncompressed size = 8
            0xFF,  // Control byte: all 8 are literals
            'H', 'e', 'l', 'l', 'o', '!', '!', '!'
        };

        auto result = szdd_extractor::extract(data);
        REQUIRE(result.has_value());
        CHECK(result->size() == 8);

        std::string text(result->begin(), result->begin() + 5);
        CHECK(text == "Hello");
    }

    TEST_CASE("Truncated header") {
        std::array<u8, 8> data = {
            'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33
        };

        auto header = szdd_extractor::parse_header(data);
        CHECK_FALSE(header.has_value());
        CHECK(header.error().code() == error_code::TruncatedArchive);
    }

    TEST_CASE("No data after header") {
        std::array<u8, 14> data = {
            'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33,
            'A', '_',
            0x10, 0x00, 0x00, 0x00  // Expects 16 bytes but none follow
        };

        auto result = szdd_extractor::extract(data);
        // Should succeed but with 0 bytes extracted
        REQUIRE(result.has_value());
        CHECK(result->size() == 0);
    }
}
