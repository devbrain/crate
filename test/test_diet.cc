#include <doctest/doctest.h>
#include <crate/compression/diet.hh>
#include <crate/test_config.hh>
#include <array>
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
            return;
        }

        diet_decompressor decompressor;

        size_t progress_calls = 0;
        size_t last_written = 0;
        decompressor.set_progress_callback([&](size_t written, size_t /*total*/) {
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

    TEST_CASE("Streaming byte by byte") {
        auto compressed = read_file(test::diet_dir() / "sprites.c");
        auto expected = read_file(test::diet_dir() / "sprites.d");

        if (compressed.empty() || expected.empty()) {
            return;
        }

        diet_decompressor decompressor;
        std::vector<u8> output(expected.size() + 1024);
        size_t in_pos = 0;
        size_t out_pos = 0;

        // Feed input in small chunks
        while (in_pos < compressed.size()) {
            size_t chunk_size = std::min<size_t>(100, compressed.size() - in_pos);
            bool is_last = (in_pos + chunk_size >= compressed.size());

            byte_span in_chunk(compressed.data() + in_pos, chunk_size);
            mutable_byte_span out_chunk(output.data() + out_pos, output.size() - out_pos);

            auto result = decompressor.decompress_some(in_chunk, out_chunk, is_last);
            REQUIRE(result.has_value());

            in_pos += result->bytes_read;
            out_pos += result->bytes_written;

            if (result->finished()) {
                break;
            }
        }

        CHECK(out_pos == expected.size());
        output.resize(out_pos);
        CHECK(output == expected);
    }

    TEST_CASE("Streaming small output buffer") {
        auto compressed = read_file(test::diet_dir() / "sprites.c");
        auto expected = read_file(test::diet_dir() / "sprites.d");

        if (compressed.empty() || expected.empty()) {
            return;
        }

        diet_decompressor decompressor;
        std::vector<u8> full_output;
        size_t in_pos = 0;

        while (true) {
            std::array<u8, 128> small_out{};
            byte_span remaining_input(compressed.data() + in_pos, compressed.size() - in_pos);

            auto result = decompressor.decompress_some(remaining_input, small_out, true);
            REQUIRE(result.has_value());

            in_pos += result->bytes_read;
            for (size_t i = 0; i < result->bytes_written; i++) {
                full_output.push_back(small_out[i]);
            }

            if (result->finished()) {
                break;
            }
        }

        CHECK(full_output.size() == expected.size());
        CHECK(full_output == expected);
    }

    TEST_CASE("Empty input") {
        diet_decompressor decompressor;
        std::vector<u8> output(1024);

        auto result = decompressor.decompress({}, output);
        // Empty input should fail
        CHECK(!result.has_value());
    }
}
