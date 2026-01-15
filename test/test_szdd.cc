#include <doctest/doctest.h>
#include <crate/compression/szdd.hh>
#include <crate/compression/lzss.hh>
#include <crate/test_config.hh>
#include "test_streaming.hh"
#include <array>
#include <vector>
#include <random>

using namespace crate;
using namespace crate::test;

// Helper to decompress SZDD data
static result_t<byte_vector> decompress_szdd(byte_span data) {
    auto header = szdd_decompressor::parse_header(data);
    if (!header) return std::unexpected(header.error());

    byte_vector output(header->uncompressed_size);
    szdd_decompressor decompressor;
    auto result = decompressor.decompress(data, output);
    if (!result) return std::unexpected(result.error());

    output.resize(*result);
    return output;
}

TEST_SUITE("SzddDecompressor") {
    TEST_CASE("Parse SZDD header - standard") {
        std::array<u8, 14> data = {
            'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33,  // Signature
            'A',  // Compression method
            'E',  // Missing character
            0x00, 0x10, 0x00, 0x00  // Uncompressed size (4096)
        };

        auto header = szdd_decompressor::parse_header(data);
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

        auto header = szdd_decompressor::parse_header(data);
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

        auto header = szdd_decompressor::parse_header(data);
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

        auto header = szdd_decompressor::parse_header(data);
        CHECK_FALSE(header.has_value());
        CHECK(header.error().code() == error_code::UnsupportedCompression);
    }

    TEST_CASE("Recover filename") {
        CHECK(szdd_decompressor::recover_filename("SETUP.EX_", 'E') == "SETUP.EXE");
        CHECK(szdd_decompressor::recover_filename("README.TX_", 'T') == "README.TXT");
        CHECK(szdd_decompressor::recover_filename("FOO.DL_", 'L') == "FOO.DLL");
        CHECK(szdd_decompressor::recover_filename("BAR.SY_", 'S') == "BAR.SYS");
    }

    TEST_CASE("Recover filename - no underscore") {
        CHECK(szdd_decompressor::recover_filename("NOUNDER.TXT", 'X') == "NOUNDER.TXT");
    }

    TEST_CASE("Recover filename - null missing char") {
        CHECK(szdd_decompressor::recover_filename("FILE.EX_", '\0') == "FILE.EX_");
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

        auto result = decompress_szdd(data);
        REQUIRE(result.has_value());
        CHECK(result->size() == 8);

        std::string text(result->begin(), result->begin() + 5);
        CHECK(text == "Hello");
    }

    TEST_CASE("Truncated header") {
        std::array<u8, 8> data = {
            'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33
        };

        auto header = szdd_decompressor::parse_header(data);
        CHECK_FALSE(header.has_value());
        CHECK(header.error().code() == error_code::TruncatedArchive);
    }

    TEST_CASE("No data after header") {
        std::array<u8, 14> data = {
            'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33,
            'A', '_',
            0x10, 0x00, 0x00, 0x00  // Expects 16 bytes but none follow
        };

        auto result = decompress_szdd(data);
        // Should succeed but with 0 bytes extracted
        REQUIRE(result.has_value());
        CHECK(result->size() == 0);
    }
}

TEST_SUITE("SzddLzssStreaming") {

    TEST_CASE("Systematic streaming - simple literals") {
        auto [compressed, expected] = generate_lzss_literals("Hello, World!");

        streaming_test_config config;
        config.name = "Simple literals";
        config.compressed = compressed;
        config.expected = expected;

        run_streaming_tests(config, []() {
            return std::make_unique<szdd_lzss_decompressor>();
        });
    }

    TEST_CASE("Systematic streaming - longer text") {
        std::string text;
        for (int i = 0; i < 100; i++) {
            text += static_cast<char>('A' + (i % 26));
        }
        auto [compressed, expected] = generate_lzss_literals(text);

        streaming_test_config config;
        config.name = "100 character text";
        config.compressed = compressed;
        config.expected = expected;

        run_streaming_tests(config, []() {
            return std::make_unique<szdd_lzss_decompressor>();
        });
    }

    TEST_CASE("Systematic streaming - with matches") {
        // 1 literal block (8 bytes) + 1 match block (8 matches * 3 bytes = 24 bytes)
        auto [compressed, expected] = generate_lzss_with_matches(1, 1);

        streaming_test_config config;
        config.name = "1 literal block + 1 match block";
        config.compressed = compressed;
        config.expected = expected;

        run_streaming_tests(config, []() {
            return std::make_unique<szdd_lzss_decompressor>();
        });
    }

    TEST_CASE("Systematic streaming - large data with matches") {
        // 2 literal blocks (16 bytes) + 3 match blocks (72 bytes)
        auto [compressed, expected] = generate_lzss_with_matches(2, 3);

        streaming_test_config config;
        config.name = "2 literal blocks + 3 match blocks";
        config.compressed = compressed;
        config.expected = expected;
        config.random_trials = 20;

        run_streaming_tests(config, []() {
            return std::make_unique<szdd_lzss_decompressor>();
        });
    }

    TEST_CASE("Streaming - output buffer constraints") {
        auto [compressed, expected] = generate_lzss_literals("ABCDEFGHIJKLMNOP");

        streaming_test_config config;
        config.name = "Output buffer constraints";
        config.compressed = compressed;
        config.expected = expected;
        config.output_buffer_sizes = {1, 2, 3, 4, 5, 8, 16};

        run_streaming_tests(config, []() {
            return std::make_unique<szdd_lzss_decompressor>();
        });
    }

    TEST_CASE("Streaming - empty input") {
        szdd_lzss_decompressor decompressor;
        byte_vector output(16);

        // Empty input, not finished
        auto result1 = decompressor.decompress_some({}, output, false);
        REQUIRE(result1.has_value());
        CHECK(result1->bytes_read == 0);
        CHECK(result1->bytes_written == 0);
        CHECK_FALSE(result1->finished());

        // Empty input, finished
        auto result2 = decompressor.decompress_some({}, output, true);
        REQUIRE(result2.has_value());
        CHECK(result2->bytes_read == 0);
        CHECK(result2->bytes_written == 0);
        CHECK(result2->finished());
    }

    TEST_CASE("Streaming - manual multi-call verification") {
        // Manual test to verify state preservation across calls
        std::vector<u8> compressed = {
            0xFF, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'
        };

        szdd_lzss_decompressor decompressor;

        // Feed 1 byte at a time
        byte_vector output(16);
        size_t out_pos = 0;

        for (size_t i = 0; i < compressed.size(); i++) {
            bool is_last = (i == compressed.size() - 1);
            byte_span in_chunk{compressed.data() + i, 1};
            mutable_byte_span out_chunk{output.data() + out_pos, output.size() - out_pos};

            auto result = decompressor.decompress_some(in_chunk, out_chunk, is_last);
            REQUIRE(result.has_value());
            out_pos += result->bytes_written;

            if (result->finished()) break;
        }

        output.resize(out_pos);
        std::string text(output.begin(), output.end());
        CHECK(text == "ABCDEFGH");
    }
}

TEST_SUITE("SzddFullStreaming") {
    // Helper to create standard SZDD data with given LZSS content
    static std::vector<u8> make_szdd_data(const std::vector<u8>& lzss_content, u32 uncompressed_size) {
        std::vector<u8> data = {
            'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33,  // Signature
            'A',  // Compression method
            '_',  // Missing character
            static_cast<u8>(uncompressed_size & 0xFF),
            static_cast<u8>((uncompressed_size >> 8) & 0xFF),
            static_cast<u8>((uncompressed_size >> 16) & 0xFF),
            static_cast<u8>((uncompressed_size >> 24) & 0xFF)
        };
        data.insert(data.end(), lzss_content.begin(), lzss_content.end());
        return data;
    }

    // Helper to create QBasic SZDD data
    static std::vector<u8> make_qbasic_szdd_data(const std::vector<u8>& lzss_content, u32 uncompressed_size) {
        std::vector<u8> data = {
            'S', 'Z', ' ', 0x88, 0xF0, 0x27, 0x33,  // QBasic signature (7 bytes)
            static_cast<u8>(uncompressed_size & 0xFF),
            static_cast<u8>((uncompressed_size >> 8) & 0xFF),
            static_cast<u8>((uncompressed_size >> 16) & 0xFF),
            static_cast<u8>((uncompressed_size >> 24) & 0xFF)
        };
        data.insert(data.end(), lzss_content.begin(), lzss_content.end());
        return data;
    }

    TEST_CASE("Full streaming - standard header with chunked input") {
        // LZSS: all 8 literals "ABCDEFGH"
        std::vector<u8> lzss_content = {0xFF, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
        auto data = make_szdd_data(lzss_content, 8);
        std::vector<u8> expected = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};

        // Test various chunk sizes
        for (size_t chunk_size : {1uz, 2uz, 3uz, 5uz, 7uz, 10uz, 14uz, 15uz, 20uz}) {
            CAPTURE(chunk_size);
            szdd_decompressor dec;
            byte_vector output(expected.size() + 16);
            size_t in_pos = 0;
            size_t out_pos = 0;

            while (in_pos < data.size()) {
                size_t chunk = std::min(chunk_size, data.size() - in_pos);
                bool is_last = (in_pos + chunk >= data.size());

                byte_span in_chunk{data.data() + in_pos, chunk};
                mutable_byte_span out_chunk{output.data() + out_pos, output.size() - out_pos};

                auto result = dec.decompress_some(in_chunk, out_chunk, is_last);
                REQUIRE(result.has_value());

                in_pos += result->bytes_read;
                out_pos += result->bytes_written;

                if (result->finished()) break;
            }

            output.resize(out_pos);
            CHECK(output == expected);
        }
    }

    TEST_CASE("Full streaming - QBasic header with chunked input") {
        // LZSS: all 8 literals
        std::vector<u8> lzss_content = {0xFF, 'Q', 'B', 'A', 'S', 'I', 'C', '!', '!'};
        auto data = make_qbasic_szdd_data(lzss_content, 8);
        std::vector<u8> expected = {'Q', 'B', 'A', 'S', 'I', 'C', '!', '!'};

        // Test various chunk sizes including split across header
        for (size_t chunk_size : {1uz, 2uz, 3uz, 5uz, 7uz, 11uz, 12uz, 15uz}) {
            CAPTURE(chunk_size);
            szdd_decompressor dec;
            byte_vector output(expected.size() + 16);
            size_t in_pos = 0;
            size_t out_pos = 0;

            while (in_pos < data.size()) {
                size_t chunk = std::min(chunk_size, data.size() - in_pos);
                bool is_last = (in_pos + chunk >= data.size());

                byte_span in_chunk{data.data() + in_pos, chunk};
                mutable_byte_span out_chunk{output.data() + out_pos, output.size() - out_pos};

                auto result = dec.decompress_some(in_chunk, out_chunk, is_last);
                REQUIRE(result.has_value());

                in_pos += result->bytes_read;
                out_pos += result->bytes_written;

                if (result->finished()) break;
            }

            output.resize(out_pos);
            CHECK(output == expected);
        }
    }

    TEST_CASE("Full streaming - header split across multiple calls") {
        // Test header buffering when header is split across calls
        std::vector<u8> lzss_content = {0xFF, 'T', 'E', 'S', 'T', '!', '!', '!', '!'};
        auto data = make_szdd_data(lzss_content, 8);
        std::vector<u8> expected = {'T', 'E', 'S', 'T', '!', '!', '!', '!'};

        szdd_decompressor dec;
        byte_vector output(expected.size() + 16);
        size_t out_pos = 0;

        // Feed 1 byte at a time
        for (size_t i = 0; i < data.size(); i++) {
            bool is_last = (i == data.size() - 1);
            byte_span in_chunk{data.data() + i, 1};
            mutable_byte_span out_chunk{output.data() + out_pos, output.size() - out_pos};

            auto result = dec.decompress_some(in_chunk, out_chunk, is_last);
            REQUIRE(result.has_value());
            CHECK(result->bytes_read == 1);
            out_pos += result->bytes_written;

            if (result->finished()) break;
        }

        output.resize(out_pos);
        CHECK(output == expected);
    }

    TEST_CASE("Full streaming - larger data through full pipeline") {
        // Create larger test data
        std::string text;
        for (int i = 0; i < 100; i++) {
            text += static_cast<char>('A' + (i % 26));
        }
        auto [lzss_content, expected] = generate_lzss_literals(text);
        auto data = make_szdd_data(lzss_content, static_cast<u32>(expected.size()));

        // Use streaming test framework approach
        for (size_t chunk_size : {1uz, 2uz, 3uz, 5uz, 7uz, 11uz, 13uz, 17uz, 32uz, 64uz}) {
            CAPTURE(chunk_size);
            szdd_decompressor dec;
            byte_vector output(expected.size() + 128);
            size_t in_pos = 0;
            size_t out_pos = 0;

            while (in_pos < data.size()) {
                size_t chunk = std::min(chunk_size, data.size() - in_pos);
                bool is_last = (in_pos + chunk >= data.size());

                byte_span in_chunk{data.data() + in_pos, chunk};
                mutable_byte_span out_chunk{output.data() + out_pos, output.size() - out_pos};

                auto result = dec.decompress_some(in_chunk, out_chunk, is_last);
                REQUIRE(result.has_value());

                in_pos += result->bytes_read;
                out_pos += result->bytes_written;

                if (result->finished()) break;
            }

            output.resize(out_pos);
            CHECK(output == expected);
        }
    }

    TEST_CASE("Full streaming - truncated header detection") {
        // Incomplete standard header
        std::vector<u8> data = {'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33};

        szdd_decompressor dec;
        byte_vector output(16);

        auto result = dec.decompress_some(data, output, true);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code() == error_code::TruncatedArchive);
    }

    TEST_CASE("Full streaming - invalid signature detection") {
        std::vector<u8> data = {
            'N', 'O', 'P', 'E', 0x88, 0xF0, 0x27, 0x33,
            'A', '_', 0x08, 0x00, 0x00, 0x00
        };

        szdd_decompressor dec;
        byte_vector output(16);

        auto result = dec.decompress_some(data, output, true);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code() == error_code::InvalidSignature);
    }

    TEST_CASE("Full streaming - random chunk sizes") {
        std::string text = "The quick brown fox jumps over the lazy dog";
        auto [lzss_content, expected] = generate_lzss_literals(text);
        auto data = make_szdd_data(lzss_content, static_cast<u32>(expected.size()));

        std::mt19937 rng(12345);
        std::uniform_int_distribution<size_t> dist(1, 10);

        for (int trial = 0; trial < 20; trial++) {
            CAPTURE(trial);
            szdd_decompressor dec;
            byte_vector output(expected.size() + 64);
            size_t in_pos = 0;
            size_t out_pos = 0;

            while (in_pos < data.size()) {
                size_t chunk = std::min(dist(rng), data.size() - in_pos);
                bool is_last = (in_pos + chunk >= data.size());

                byte_span in_chunk{data.data() + in_pos, chunk};
                mutable_byte_span out_chunk{output.data() + out_pos, output.size() - out_pos};

                auto result = dec.decompress_some(in_chunk, out_chunk, is_last);
                REQUIRE(result.has_value());

                in_pos += result->bytes_read;
                out_pos += result->bytes_written;

                if (result->finished()) break;
            }

            output.resize(out_pos);
            CHECK(output == expected);
        }
    }

    TEST_CASE("Full streaming - reset and reuse") {
        std::vector<u8> lzss_content = {0xFF, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
        auto data = make_szdd_data(lzss_content, 8);
        std::vector<u8> expected = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};

        szdd_decompressor dec;

        // First decompression
        byte_vector output1(expected.size() + 16);
        auto result1 = dec.decompress(data, output1);
        REQUIRE(result1.has_value());
        output1.resize(*result1);
        CHECK(output1 == expected);

        // Reset and decompress again
        dec.reset();
        byte_vector output2(expected.size() + 16);
        auto result2 = dec.decompress(data, output2);
        REQUIRE(result2.has_value());
        output2.resize(*result2);
        CHECK(output2 == expected);
    }
}
