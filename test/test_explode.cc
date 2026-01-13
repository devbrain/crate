#include <doctest/doctest.h>
#include <crate/compression/explode.hh>
#include <crate/test_config.hh>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace crate;

namespace {

std::vector<u8> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::vector<u8>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    );
}

} // anonymous namespace

TEST_SUITE("ExplodeDecompressor") {
    TEST_CASE("Basic functionality") {
        explode_decompressor decompressor;
        CHECK_NOTHROW(decompressor.reset());
    }

    TEST_CASE("Decompress small file") {
        auto compressed = read_file(test::pkware_dir() / "small.imploded");
        auto expected = read_file(test::pkware_dir() / "small.decomp");

        if (compressed.empty() || expected.empty()) {
            MESSAGE("Test data not found, skipping");
            return;
        }

        explode_decompressor decompressor;
        std::vector<u8> output(expected.size() + 1024);

        auto result = decompressor.decompress(compressed, output);
        REQUIRE(result.has_value());
        CHECK(*result == expected.size());

        output.resize(*result);
        CHECK(output == expected);
    }

    TEST_CASE("Decompress medium file") {
        auto compressed = read_file(test::pkware_dir() / "medium.imploded");
        auto expected = read_file(test::pkware_dir() / "medium.decomp");

        if (compressed.empty() || expected.empty()) {
            MESSAGE("Test data not found, skipping");
            return;
        }

        explode_decompressor decompressor;
        std::vector<u8> output(expected.size() + 1024);

        auto result = decompressor.decompress(compressed, output);
        REQUIRE(result.has_value());
        CHECK(*result == expected.size());

        output.resize(*result);
        CHECK(output == expected);
    }

    TEST_CASE("Decompress large file") {
        auto compressed = read_file(test::pkware_dir() / "large.imploded");
        auto expected = read_file(test::pkware_dir() / "large.decomp");

        if (compressed.empty() || expected.empty()) {
            MESSAGE("Test data not found, skipping");
            return;
        }

        explode_decompressor decompressor;
        std::vector<u8> output(expected.size() + 1024);

        auto result = decompressor.decompress(compressed, output);
        REQUIRE(result.has_value());
        CHECK(*result == expected.size());

        output.resize(*result);
        CHECK(output == expected);
    }

    TEST_CASE("Decompress binary file") {
        auto compressed = read_file(test::pkware_dir() / "binary.imploded");
        auto expected = read_file(test::pkware_dir() / "binary.decomp");

        if (compressed.empty() || expected.empty()) {
            MESSAGE("Test data not found, skipping");
            return;
        }

        explode_decompressor decompressor;
        std::vector<u8> output(expected.size() + 1024);

        auto result = decompressor.decompress(compressed, output);
        REQUIRE(result.has_value());
        CHECK(*result == expected.size());

        output.resize(*result);
        CHECK(output == expected);
    }

    TEST_CASE("Decompress file without explicit end marker") {
        auto compressed = read_file(test::pkware_dir() / "no-explicit-end.imploded");
        auto expected = read_file(test::pkware_dir() / "no-explicit-end.decomp");

        if (compressed.empty() || expected.empty()) {
            MESSAGE("Test data not found, skipping");
            return;
        }

        explode_decompressor decompressor;
        std::vector<u8> output(expected.size() + 1024);

        auto result = decompressor.decompress(compressed, output);
        REQUIRE(result.has_value());
        CHECK(*result == expected.size());

        output.resize(*result);
        CHECK(output == expected);
    }

    TEST_CASE("Invalid dictionary size") {
        // Create invalid data with dict size = 7 (valid is 4-6)
        std::vector<u8> data = {0x00, 0x07, 0x00};  // binary mode, dict size 7
        std::vector<u8> output(1024);

        explode_decompressor decompressor;
        auto result = decompressor.decompress(data, output);
        CHECK(!result.has_value());
    }

    TEST_CASE("Invalid compression type") {
        // Create invalid data with compression type = 2 (valid is 0 or 1)
        std::vector<u8> data = {0x02, 0x04, 0x00};  // type 2, dict size 4
        std::vector<u8> output(1024);

        explode_decompressor decompressor;
        auto result = decompressor.decompress(data, output);
        CHECK(!result.has_value());
    }

    TEST_CASE("Input too short") {
        std::vector<u8> data = {0x00, 0x04};  // Only 2 bytes, need at least 3
        std::vector<u8> output(1024);

        explode_decompressor decompressor;
        auto result = decompressor.decompress(data, output);
        CHECK(!result.has_value());
    }
}
