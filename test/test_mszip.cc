#include <doctest/doctest.h>
#include <crate/compression/mszip.hh>
#include <array>
#include <vector>

using namespace crate;

TEST_SUITE("MszipDecompressor") {
    TEST_CASE("Decompress stored block") {
        // MSZIP with stored block
        std::vector<u8> data = {
            'C', 'K',  // MSZIP signature
            0x01,      // Final block, type 0 (stored)
            0x05, 0x00,  // Length = 5
            0xFA, 0xFF,  // ~Length (one's complement)
            'H', 'e', 'l', 'l', 'o'
        };

        mszip_decompressor decompressor;
        std::array<u8, 16> output{};

        auto result = decompressor.decompress(data, output);
        REQUIRE(result.has_value());
        CHECK(*result == 5);

        std::string text(output.begin(), output.begin() + 5);
        CHECK(text == "Hello");
    }

    TEST_CASE("Missing signature") {
        std::vector<u8> data = {'X', 'Y', 0x01, 0x05, 0x00, 0xFA, 0xFF, 'H', 'e', 'l', 'l', 'o'};

        mszip_decompressor decompressor;
        std::array<u8, 16> output{};

        auto result = decompressor.decompress(data, output);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code() == error_code::InvalidSignature);
    }

    TEST_CASE("Invalid stored block length") {
        std::vector<u8> data = {
            'C', 'K',
            0x01,        // Final block, type 0 (stored)
            0x05, 0x00,  // Length = 5
            0x00, 0x00,  // Invalid complement (should be 0xFA, 0xFF)
            'H', 'e', 'l', 'l', 'o'
        };

        mszip_decompressor decompressor;
        std::array<u8, 16> output{};

        auto result = decompressor.decompress(data, output);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code() == error_code::CorruptData);
    }

    TEST_CASE("Reset state between blocks") {
        std::vector<u8> data = {
            'C', 'K',
            0x01, 0x03, 0x00, 0xFC, 0xFF, 'A', 'B', 'C'
        };

        mszip_decompressor decompressor;

        // First decompression
        std::array<u8, 16> output1{};
        auto result1 = decompressor.decompress(data, output1);
        REQUIRE(result1.has_value());

        // Reset and decompress again
        decompressor.reset();
        std::array<u8, 16> output2{};
        auto result2 = decompressor.decompress(data, output2);
        REQUIRE(result2.has_value());

        CHECK(*result1 == *result2);
        CHECK(std::equal(output1.begin(), output1.begin() + *result1,
                         output2.begin()));
    }

    TEST_CASE("Empty input") {
        std::vector<u8> data = {};

        mszip_decompressor decompressor;
        std::array<u8, 16> output{};

        auto result = decompressor.decompress(data, output);
        CHECK_FALSE(result.has_value());
    }

    TEST_CASE("Reserved block type 3") {
        std::vector<u8> data = {
            'C', 'K',
            0x07  // Final block, type 3 (reserved/invalid)
        };

        mszip_decompressor decompressor;
        std::array<u8, 16> output{};

        auto result = decompressor.decompress(data, output);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code() == error_code::InvalidBlockType);
    }

    TEST_CASE("Streaming decompression - byte by byte") {
        // MSZIP with stored block
        std::vector<u8> data = {
            'C', 'K',  // MSZIP signature
            0x01,      // Final block, type 0 (stored)
            0x05, 0x00,  // Length = 5
            0xFA, 0xFF,  // ~Length (one's complement)
            'H', 'e', 'l', 'l', 'o'
        };

        mszip_decompressor decompressor;
        std::vector<u8> output(16);
        size_t total_in = 0;
        size_t total_out = 0;

        // Feed one byte at a time
        for (size_t i = 0; i < data.size(); i++) {
            bool is_last = (i == data.size() - 1);
            byte_span input_chunk(data.data() + i, 1);
            mutable_byte_span output_chunk(output.data() + total_out, output.size() - total_out);

            auto result = decompressor.decompress_some(input_chunk, output_chunk, is_last);
            REQUIRE(result.has_value());

            total_in += result->bytes_read;
            total_out += result->bytes_written;

            if (result->finished()) {
                break;
            }

            // Not finished yet - should need more input or output
            CHECK((result->status == decode_status::needs_more_input ||
                   result->status == decode_status::needs_more_output));
        }

        CHECK(total_out == 5);
        std::string text(output.begin(), output.begin() + 5);
        CHECK(text == "Hello");
    }

    TEST_CASE("Streaming - small output buffer") {
        std::vector<u8> data = {
            'C', 'K',  // MSZIP signature
            0x01,      // Final block, type 0 (stored)
            0x05, 0x00,  // Length = 5
            0xFA, 0xFF,  // ~Length
            'H', 'e', 'l', 'l', 'o'
        };

        mszip_decompressor decompressor;
        std::vector<u8> full_output;

        size_t in_pos = 0;
        while (in_pos < data.size() || !full_output.empty()) {
            // Small output buffer - only 2 bytes at a time
            std::array<u8, 2> small_out{};
            byte_span remaining_input(data.data() + in_pos, data.size() - in_pos);

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

        CHECK(full_output.size() == 5);
        std::string text(full_output.begin(), full_output.end());
        CHECK(text == "Hello");
    }
}
