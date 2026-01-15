#include <doctest/doctest.h>
#include <crate/compression/diet.hh>
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

TEST_SUITE("DietDecompressor") {
    TEST_CASE("Basic functionality") {
        diet_decompressor decompressor;
        CHECK_NOTHROW(decompressor.reset());
    }

    TEST_CASE("Decompress sprites.c") {
        auto compressed = read_file(test::diet_dir() / "sprites.c");
        auto expected = read_file(test::diet_dir() / "sprites.d");

        if (compressed.empty() || expected.empty()) {
            MESSAGE("Test data not found, skipping");
            return;
        }

        INFO("Compressed size: " << compressed.size());
        INFO("Expected size: " << expected.size());

        diet_decompressor decompressor;
        std::vector<u8> output(expected.size() + 1024);

        auto result = decompressor.decompress(compressed, output);
        REQUIRE(result.has_value());
        CHECK(*result == expected.size());

        output.resize(*result);
        CHECK(output == expected);
    }

    TEST_CASE("Decompress with progress callback") {
        auto compressed = read_file(test::diet_dir() / "sprites.c");
        auto expected = read_file(test::diet_dir() / "sprites.d");

        if (compressed.empty() || expected.empty()) {
            MESSAGE("Test data not found, skipping");
            return;
        }

        diet_decompressor decompressor;

        size_t progress_calls = 0;
        size_t last_written = 0;
        decompressor.set_progress_callback([&](size_t written, size_t total) {
            CHECK(written >= last_written);  // Progress should not go backward
            last_written = written;
            progress_calls++;
        });

        std::vector<u8> output(expected.size() + 1024);
        auto result = decompressor.decompress(compressed, output);

        REQUIRE(result.has_value());
        CHECK(*result == expected.size());
        // Progress callback should have been called at least once
        CHECK(progress_calls >= 1);
    }

    TEST_CASE("Reset and reuse") {
        auto compressed = read_file(test::diet_dir() / "sprites.c");
        auto expected = read_file(test::diet_dir() / "sprites.d");

        if (compressed.empty() || expected.empty()) {
            MESSAGE("Test data not found, skipping");
            return;
        }

        diet_decompressor decompressor;

        // First decompression
        std::vector<u8> output1(expected.size() + 1024);
        auto result1 = decompressor.decompress(compressed, output1);
        REQUIRE(result1.has_value());
        output1.resize(*result1);
        CHECK(output1 == expected);

        // Reset and decompress again
        decompressor.reset();
        std::vector<u8> output2(expected.size() + 1024);
        auto result2 = decompressor.decompress(compressed, output2);
        REQUIRE(result2.has_value());
        output2.resize(*result2);
        CHECK(output2 == expected);
    }

    TEST_CASE("Streaming requires all input") {
        auto compressed = read_file(test::diet_dir() / "sprites.c");

        if (compressed.empty()) {
            MESSAGE("Test data not found, skipping");
            return;
        }

        diet_decompressor decompressor;
        std::vector<u8> output(256 * 1024);

        // Call with input_finished=false should return needs_more_input
        auto result = decompressor.decompress_some(compressed, output, false);
        REQUIRE(result.has_value());
        CHECK(result->status == decode_status::needs_more_input);
        CHECK(result->bytes_read == 0);
        CHECK(result->bytes_written == 0);
    }

    TEST_CASE("Empty input") {
        diet_decompressor decompressor;
        std::vector<u8> output(1024);

        auto result = decompressor.decompress({}, output);
        // Empty input should fail
        CHECK(!result.has_value());
    }
}
