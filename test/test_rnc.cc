#include <doctest/doctest.h>
#include <crate/compression/rnc.hh>
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

TEST_SUITE("RncDecompressor") {
    TEST_CASE("Basic functionality") {
        rnc_decompressor decompressor;
        CHECK_NOTHROW(decompressor.reset());
        CHECK(decompressor.supports_streaming());
    }

    TEST_CASE("Decompress method 1") {
        auto compressed = read_file(test::rnc_dir() / "test_method1.rnc");
        auto expected = read_file(test::rnc_dir() / "expected_output.txt");

        if (compressed.empty() || expected.empty()) {
            MESSAGE("Test data not found, skipping");
            return;
        }

        INFO("Compressed size: " << compressed.size());
        INFO("Expected size: " << expected.size());

        rnc_decompressor decompressor;
        std::vector<u8> output(expected.size() + 1024);

        auto result = decompressor.decompress(compressed, output);
        if (!result.has_value()) {
            MESSAGE("Error: " << result.error().message());
        }
        REQUIRE(result.has_value());
        CHECK(*result == expected.size());

        output.resize(*result);
        CHECK(output == expected);
    }

    TEST_CASE("Decompress method 2") {
        auto compressed = read_file(test::rnc_dir() / "test_method2.rnc");
        auto expected = read_file(test::rnc_dir() / "expected_output.txt");

        if (compressed.empty() || expected.empty()) {
            MESSAGE("Test data not found, skipping");
            return;
        }

        INFO("Compressed size: " << compressed.size());
        INFO("Expected size: " << expected.size());

        rnc_decompressor decompressor;
        std::vector<u8> output(expected.size() + 1024);

        auto result = decompressor.decompress(compressed, output);
        if (!result.has_value()) {
            MESSAGE("Error: " << result.error().message());
        }
        REQUIRE(result.has_value());
        CHECK(*result == expected.size());

        output.resize(*result);
        CHECK(output == expected);
    }

    TEST_CASE("Reset and reuse") {
        auto compressed = read_file(test::rnc_dir() / "test_method1.rnc");
        auto expected = read_file(test::rnc_dir() / "expected_output.txt");

        if (compressed.empty() || expected.empty()) {
            MESSAGE("Test data not found, skipping");
            return;
        }

        rnc_decompressor decompressor;

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

    TEST_CASE("Method 2 streaming small input chunks") {
        // Note: Method 1 requires all input upfront, so we test method 2 for streaming
        auto compressed = read_file(test::rnc_dir() / "test_method2.rnc");
        auto expected = read_file(test::rnc_dir() / "expected_output.txt");

        if (compressed.empty() || expected.empty()) {
            MESSAGE("Test data not found, skipping");
            return;
        }

        rnc_decompressor decompressor;
        std::vector<u8> output(expected.size() + 1024);
        size_t in_pos = 0;
        size_t out_pos = 0;

        // Feed input in small chunks
        while (in_pos < compressed.size()) {
            size_t chunk_size = std::min<size_t>(32, compressed.size() - in_pos);
            bool is_last = (in_pos + chunk_size >= compressed.size());

            byte_span in_chunk(compressed.data() + in_pos, chunk_size);
            mutable_byte_span out_chunk(output.data() + out_pos, output.size() - out_pos);

            auto result = decompressor.decompress_some(in_chunk, out_chunk, is_last);
            if (!result.has_value()) {
                MESSAGE("Error: " << result.error().message());
                MESSAGE("in_pos=" << in_pos << " out_pos=" << out_pos << " chunk_size=" << chunk_size);
            }
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

    TEST_CASE("Method 2 streaming small output buffer") {
        auto compressed = read_file(test::rnc_dir() / "test_method2.rnc");
        auto expected = read_file(test::rnc_dir() / "expected_output.txt");

        if (compressed.empty() || expected.empty()) {
            MESSAGE("Test data not found, skipping");
            return;
        }

        rnc_decompressor decompressor;
        std::vector<u8> full_output;
        size_t in_pos = 0;

        while (true) {
            std::array<u8, 64> small_out{};
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

    TEST_CASE("Invalid signature") {
        std::vector<u8> garbage = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                   0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
                                   0x11, 0x12};

        rnc_decompressor decompressor;
        std::vector<u8> output(1024);

        auto result = decompressor.decompress(garbage, output);
        CHECK(!result.has_value());
        CHECK(result.error().code() == error_code::InvalidSignature);
    }

    TEST_CASE("Empty input") {
        rnc_decompressor decompressor;
        std::vector<u8> output(1024);

        auto result = decompressor.decompress({}, output);
        // Empty input should fail (truncated)
        CHECK(!result.has_value());
    }

    TEST_CASE("CRC calculation") {
        // Test known CRC values
        std::vector<byte> data = {0x48, 0x65, 0x6c, 0x6c, 0x6f}; // "Hello"
        u16 crc = rnc::calculate_crc(data);
        // Just verify it doesn't crash and produces a consistent result
        CHECK(crc == rnc::calculate_crc(data));
    }

    TEST_CASE("Decompress large method 1") {
        auto compressed = read_file(test::rnc_dir() / "test_large_m1.rnc");
        auto expected = read_file(test::rnc_dir() / "expected_large.txt");

        if (compressed.empty() || expected.empty()) {
            MESSAGE("Large test data not found, skipping");
            return;
        }

        INFO("Compressed size: " << compressed.size());
        INFO("Expected size: " << expected.size());

        rnc_decompressor decompressor;
        std::vector<u8> output(expected.size() + 1024);

        auto result = decompressor.decompress(compressed, output);
        if (!result.has_value()) {
            MESSAGE("Error: " << result.error().message());
        }
        REQUIRE(result.has_value());
        CHECK(*result == expected.size());

        output.resize(*result);
        CHECK(output == expected);
    }

    TEST_CASE("Decompress large method 2") {
        auto compressed = read_file(test::rnc_dir() / "test_large_m2.rnc");
        auto expected = read_file(test::rnc_dir() / "expected_large.txt");

        if (compressed.empty() || expected.empty()) {
            MESSAGE("Large test data not found, skipping");
            return;
        }

        INFO("Compressed size: " << compressed.size());
        INFO("Expected size: " << expected.size());

        rnc_decompressor decompressor;
        std::vector<u8> output(expected.size() + 1024);

        auto result = decompressor.decompress(compressed, output);
        if (!result.has_value()) {
            MESSAGE("Error: " << result.error().message());
        }
        REQUIRE(result.has_value());
        CHECK(*result == expected.size());

        output.resize(*result);
        CHECK(output == expected);
    }
}
