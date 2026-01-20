#include <doctest/doctest.h>
#include <crate/compression/kwaj.hh>
#include <crate/core/system.hh>
#include <crate/test_config.hh>
#include "test_streaming.hh"
#include <array>
#include <vector>

using namespace crate;

// Helper to decompress KWAJ data
static result_t<byte_vector> decompress_kwaj(byte_span data) {
    auto header = kwaj_decompressor::parse_header(data);
    if (!header) return std::unexpected(header.error());

    // Estimate output size - use decompressed_len if available, otherwise use a reasonable default
    size_t output_size = header->decompressed_len > 0 ? header->decompressed_len : data.size() * 10;
    if (output_size == 0) output_size = data.size();

    byte_vector output(output_size);
    kwaj_decompressor decompressor;
    auto result = decompressor.decompress(data, output);
    if (!result) return std::unexpected(result.error());

    output.resize(*result);
    return output;
}

static void append_u16_le(std::vector<u8>& data, u16 value) {
    data.push_back(static_cast<u8>(value & 0xFF));
    data.push_back(static_cast<u8>((value >> 8) & 0xFF));
}

static void append_u32_le(std::vector<u8>& data, u32 value) {
    data.push_back(static_cast<u8>(value & 0xFF));
    data.push_back(static_cast<u8>((value >> 8) & 0xFF));
    data.push_back(static_cast<u8>((value >> 16) & 0xFF));
    data.push_back(static_cast<u8>((value >> 24) & 0xFF));
}

static std::vector<u8> make_kwaj_data(
    kwaj::method method,
    const std::vector<u8>& payload,
    u32 decompressed_len
) {
    std::vector<u8> data = {
        'K', 'W', 'A', 'J',
        0x88, 0xF0, 0x27, 0x33
    };

    u16 flags = 0;
    u16 data_offset = 14;
    if (decompressed_len > 0) {
        flags |= kwaj::HAS_DECOMPRESSED_LEN;
        data_offset += 4;
    }

    append_u16_le(data, static_cast<u16>(method));
    append_u16_le(data, data_offset);
    append_u16_le(data, flags);

    if (flags & kwaj::HAS_DECOMPRESSED_LEN) {
        append_u32_le(data, decompressed_len);
    }

    data.insert(data.end(), payload.begin(), payload.end());
    return data;
}

TEST_SUITE("KwajDecompressor - Basic") {
    TEST_CASE("Parse KWAJ header - basic") {
        std::array<u8, 14> data = {
            'K', 'W', 'A', 'J',
            0x88, 0xF0, 0x27, 0x33,
            0x00, 0x00,  // Method: none
            0x0E, 0x00,  // Data offset: 14
            0x00, 0x00   // Flags: none
        };

        auto header = kwaj_decompressor::parse_header(data);
        REQUIRE(header.has_value());
        CHECK(header->comp_method == 0);
        CHECK(header->data_offset == 14);
        CHECK(header->flags == 0);
    }

    TEST_CASE("Parse KWAJ header with flags") {
        std::vector<u8> data = {
            'K', 'W', 'A', 'J',
            0x88, 0xF0, 0x27, 0x33,
            0x00, 0x00,  // Method: none
            0x1B, 0x00,  // Data offset: 27 (14 base + 4 uncomp + 4 decomp + 5 filename with null)
            0x0D, 0x00,  // Flags: HAS_UNCOMPRESSED_LEN | HAS_DECOMPRESSED_LEN | HAS_FILENAME
            // Uncompressed len (4 bytes)
            0x00, 0x10, 0x00, 0x00,  // 4096
            // Decompressed len (4 bytes)
            0x00, 0x10, 0x00, 0x00,  // 4096
            // Filename (null-terminated string)
            'T', 'E', 'S', 'T', 0x00
        };

        auto header = kwaj_decompressor::parse_header(data);
        REQUIRE(header.has_value());
        CHECK(header->comp_method == 0);
        CHECK(header->uncompressed_len == 4096);
        CHECK(header->decompressed_len == 4096);
        CHECK(header->filename == "TEST");
    }

    TEST_CASE("Invalid signature - first part") {
        std::array<u8, 14> data = {
            'N', 'O', 'P', 'E',
            0x88, 0xF0, 0x27, 0x33,
            0x00, 0x00, 0x0E, 0x00, 0x00, 0x00
        };

        auto header = kwaj_decompressor::parse_header(data);
        CHECK_FALSE(header.has_value());
        CHECK(header.error().code() == error_code::InvalidSignature);
    }

    TEST_CASE("Invalid signature - second part") {
        std::array<u8, 14> data = {
            'K', 'W', 'A', 'J',
            0x00, 0x00, 0x00, 0x00,  // Wrong second signature
            0x00, 0x00, 0x0E, 0x00, 0x00, 0x00
        };

        auto header = kwaj_decompressor::parse_header(data);
        CHECK_FALSE(header.has_value());
        CHECK(header.error().code() == error_code::InvalidSignature);
    }

    TEST_CASE("Extract uncompressed KWAJ") {
        std::vector<u8> data = {
            'K', 'W', 'A', 'J',
            0x88, 0xF0, 0x27, 0x33,
            0x00, 0x00,  // Method: none
            0x0E, 0x00,  // Data offset
            0x00, 0x00,  // Flags
            'T', 'E', 'S', 'T'  // Uncompressed data
        };

        auto result = decompress_kwaj(data);
        REQUIRE(result.has_value());
        CHECK(result->size() == 4);

        std::string text(result->begin(), result->end());
        CHECK(text == "TEST");
    }

    TEST_CASE("Extract XOR-compressed KWAJ") {
        std::vector<u8> data = {
            'K', 'W', 'A', 'J',
            0x88, 0xF0, 0x27, 0x33,
            0x01, 0x00,  // Method: XOR with 0xFF
            0x0E, 0x00,  // Data offset
            0x00, 0x00,  // Flags
            static_cast<u8>('T' ^ 0xFF),
            static_cast<u8>('E' ^ 0xFF),
            static_cast<u8>('S' ^ 0xFF),
            static_cast<u8>('T' ^ 0xFF)
        };

        auto result = decompress_kwaj(data);
        REQUIRE(result.has_value());
        CHECK(result->size() == 4);

        std::string text(result->begin(), result->end());
        CHECK(text == "TEST");
    }

    TEST_CASE("Truncated header") {
        std::array<u8, 8> data = {
            'K', 'W', 'A', 'J',
            0x88, 0xF0, 0x27, 0x33
        };

        auto header = kwaj_decompressor::parse_header(data);
        CHECK_FALSE(header.has_value());
        CHECK(header.error().code() == error_code::TruncatedArchive);
    }

    TEST_CASE("Data offset past end") {
        std::vector<u8> data = {
            'K', 'W', 'A', 'J',
            0x88, 0xF0, 0x27, 0x33,
            0x00, 0x00,
            0xFF, 0x00,  // Data offset: 255 (past end)
            0x00, 0x00
        };

        auto result = decompress_kwaj(data);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code() == error_code::TruncatedArchive);
    }
}

// Helper to load a KWAJ file
static result_t<byte_vector> load_kwaj_file(const std::filesystem::path& path) {
    auto file = file_input_stream::open(path);
    if (!file) return std::unexpected(file.error());

    auto size = file->size();
    if (!size) return std::unexpected(size.error());

    byte_vector data(*size);
    auto read = file->read(data);
    if (!read) return std::unexpected(read.error());

    return data;
}

TEST_SUITE("KwajDecompressor - Ground Truth Tests") {
    // Ground truth from kwajd_test.c:
    // fXY.kwj where X = filename length (0-8), Y = extension variation
    // Y=0: no extension, Y=1: ".1", Y=2: ".12", Y=3: ".123", Y=4: should FAIL
    // X=9: all should FAIL

    TEST_CASE("f00 - no filename, no extension") {
        auto path = ::test::testdata_dir() / "kwaj" / "f00.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        REQUIRE(header.has_value());
        CHECK(header->filename.empty());
        CHECK(header->extension.empty());
    }

    TEST_CASE("f01 - extension only: .1") {
        auto path = ::test::testdata_dir() / "kwaj" / "f01.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        REQUIRE(header.has_value());
        CHECK(header->extension == "1");
    }

    TEST_CASE("f02 - extension only: .12") {
        auto path = ::test::testdata_dir() / "kwaj" / "f02.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        REQUIRE(header.has_value());
        CHECK(header->extension == "12");
    }

    TEST_CASE("f03 - extension only: .123") {
        auto path = ::test::testdata_dir() / "kwaj" / "f03.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        REQUIRE(header.has_value());
        CHECK(header->extension == "123");
    }

    TEST_CASE("f10 - filename only: 1") {
        auto path = ::test::testdata_dir() / "kwaj" / "f10.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        REQUIRE(header.has_value());
        CHECK(header->filename == "1");
        CHECK(header->extension.empty());
    }

    TEST_CASE("f11 - filename and extension: 1.1") {
        auto path = ::test::testdata_dir() / "kwaj" / "f11.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        REQUIRE(header.has_value());
        CHECK(header->filename == "1");
        CHECK(header->extension == "1");
    }

    TEST_CASE("f20 - filename: 12") {
        auto path = ::test::testdata_dir() / "kwaj" / "f20.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        REQUIRE(header.has_value());
        CHECK(header->filename == "12");
    }

    TEST_CASE("f21 - filename and extension: 12.1") {
        auto path = ::test::testdata_dir() / "kwaj" / "f21.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        REQUIRE(header.has_value());
        CHECK(header->filename == "12");
        CHECK(header->extension == "1");
    }

    TEST_CASE("f80 - filename: 12345678") {
        auto path = ::test::testdata_dir() / "kwaj" / "f80.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        REQUIRE(header.has_value());
        CHECK(header->filename == "12345678");
    }

    TEST_CASE("f83 - filename and extension: 12345678.123") {
        auto path = ::test::testdata_dir() / "kwaj" / "f83.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        REQUIRE(header.has_value());
        CHECK(header->filename == "12345678");
        CHECK(header->extension == "123");
    }
}

TEST_SUITE("KwajDecompressor - Files That Should Fail") {
    // According to kwajd_test.c, these files should fail with DATAFORMAT error:
    // - f04, f14, f24, f34, f44, f54, f64, f74, f84 (Y=4)
    // - f90, f91, f92, f93, f94 (X=9)
    // - cve-2018-14681.kwj

    TEST_CASE("f04 - should fail") {
        auto path = ::test::testdata_dir() / "kwaj" / "f04.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        CHECK_FALSE(header.has_value());
    }

    TEST_CASE("f14 - should fail") {
        auto path = ::test::testdata_dir() / "kwaj" / "f14.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        CHECK_FALSE(header.has_value());
    }

    TEST_CASE("f24 - should fail") {
        auto path = ::test::testdata_dir() / "kwaj" / "f24.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        CHECK_FALSE(header.has_value());
    }

    TEST_CASE("f34 - should fail") {
        auto path = ::test::testdata_dir() / "kwaj" / "f34.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        CHECK_FALSE(header.has_value());
    }

    TEST_CASE("f44 - should fail") {
        auto path = ::test::testdata_dir() / "kwaj" / "f44.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        CHECK_FALSE(header.has_value());
    }

    TEST_CASE("f54 - should fail") {
        auto path = ::test::testdata_dir() / "kwaj" / "f54.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        CHECK_FALSE(header.has_value());
    }

    TEST_CASE("f64 - should fail") {
        auto path = ::test::testdata_dir() / "kwaj" / "f64.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        CHECK_FALSE(header.has_value());
    }

    TEST_CASE("f74 - should fail") {
        auto path = ::test::testdata_dir() / "kwaj" / "f74.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        CHECK_FALSE(header.has_value());
    }

    TEST_CASE("f84 - should fail") {
        auto path = ::test::testdata_dir() / "kwaj" / "f84.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        CHECK_FALSE(header.has_value());
    }

    TEST_CASE("f90 - should fail") {
        auto path = ::test::testdata_dir() / "kwaj" / "f90.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        CHECK_FALSE(header.has_value());
    }

    TEST_CASE("f91 - should fail") {
        auto path = ::test::testdata_dir() / "kwaj" / "f91.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        CHECK_FALSE(header.has_value());
    }

    TEST_CASE("f92 - should fail") {
        auto path = ::test::testdata_dir() / "kwaj" / "f92.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        CHECK_FALSE(header.has_value());
    }

    TEST_CASE("f93 - should fail") {
        auto path = ::test::testdata_dir() / "kwaj" / "f93.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        CHECK_FALSE(header.has_value());
    }

    TEST_CASE("f94 - should fail") {
        auto path = ::test::testdata_dir() / "kwaj" / "f94.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        auto header = kwaj_decompressor::parse_header(*data);
        CHECK_FALSE(header.has_value());
    }

    TEST_CASE("CVE-2018-14681 - should fail") {
        auto path = ::test::testdata_dir() / "kwaj" / "cve-2018-14681.kwj";
        REQUIRE(std::filesystem::exists(path));

        auto data = load_kwaj_file(path);
        REQUIRE(data.has_value());

        // Should fail to parse - this is a malformed file
        auto header = kwaj_decompressor::parse_header(*data);
        CHECK_FALSE(header.has_value());
    }
}

TEST_SUITE("KwajDecompressor - Streaming") {
    TEST_CASE("Streaming uncompressed payload") {
        std::string text = "TEST";
        std::vector<u8> payload(text.begin(), text.end());
        auto data = make_kwaj_data(kwaj::NONE, payload, static_cast<u32>(payload.size()));

        crate::test::streaming_test_config config;
        config.name = "KWAJ none";
        config.compressed = data;
        config.expected = payload;
        config.output_buffer_sizes = {1, 2, 3};

        crate::test::run_streaming_tests(config, []() {
            return std::make_unique<kwaj_decompressor>();
        });
    }

    TEST_CASE("Streaming XOR payload") {
        std::string text = "KWAJ";
        std::vector<u8> payload;
        for (char ch : text) {
            payload.push_back(static_cast<u8>(ch ^ 0xFF));
        }
        auto data = make_kwaj_data(kwaj::XOR_FF, payload, static_cast<u32>(text.size()));

        crate::test::streaming_test_config config;
        config.name = "KWAJ xor";
        config.compressed = data;
        config.expected.assign(text.begin(), text.end());
        config.output_buffer_sizes = {1, 2, 4};

        crate::test::run_streaming_tests(config, []() {
            return std::make_unique<kwaj_decompressor>();
        });
    }

    TEST_CASE("Streaming SZDD payload") {
        std::string text = "Hello KWAJ SZDD";
        auto [lzss_payload, expected] = crate::test::generate_lzss_literals(text);
        auto data = make_kwaj_data(
            kwaj::SZDD,
            lzss_payload,
            static_cast<u32>(expected.size())
        );

        crate::test::streaming_test_config config;
        config.name = "KWAJ szdd";
        config.compressed = data;
        config.expected = expected;
        config.output_buffer_sizes = {1, 4, 7, 16};
        config.random_trials = 5;

        crate::test::run_streaming_tests(config, []() {
            return std::make_unique<kwaj_decompressor>();
        });
    }

    TEST_CASE("Streaming LZH payload") {
        std::vector <u8> expected = {'L', 'Z', 'H', 'S', 'T', 'R', 'M', '!'};
        auto lzss_payload = crate::test::make_kwaj_lzss_literals(expected);
        auto data = make_kwaj_data(
            kwaj::LZH,
            lzss_payload,
            static_cast<u32>(expected.size())
        );

        crate::test::streaming_test_config config;
        config.name = "KWAJ lzh";
        config.compressed = data;
        config.expected = expected;
        config.output_buffer_sizes = {1, 4, 9};
        config.random_trials = 5;

        crate::test::run_streaming_tests(config, []() {
            return std::make_unique<kwaj_decompressor>();
        });
    }

    TEST_CASE("decompress_stream helper") {
        std::string text = "STREAM";
        std::vector<u8> payload(text.begin(), text.end());
        auto data = make_kwaj_data(kwaj::NONE, payload, static_cast<u32>(payload.size()));

        memory_input_stream input(byte_span{data});
        vector_output_stream output;
        kwaj_decompressor decompressor;

        auto result = decompressor.decompress_stream(input, output);
        REQUIRE(result.has_value());
        CHECK(*result == payload.size());
        CHECK(output.data() == payload);
    }
}

TEST_SUITE("KwajDecompressor - Valid Files Comprehensive") {
    // Test all valid fXY files (where Y != 4 and X != 9)
    TEST_CASE("All valid KWAJ files parse and extract correctly") {
        // Expected filenames based on ground truth
        // X = filename length, Y = extension length (0=none, 1=.1, 2=.12, 3=.123)
        const char* expected_filenames[] = {
            "",         // 0
            "1",        // 1
            "12",       // 2
            "123",      // 3
            "1234",     // 4
            "12345",    // 5
            "123456",   // 6
            "1234567",  // 7
            "12345678"  // 8
        };
        const char* expected_extensions[] = {
            "",     // 0
            "1",    // 1
            "12",   // 2
            "123"   // 3
        };

        for (int x = 0; x <= 8; x++) {
            for (int y = 0; y <= 3; y++) {
                std::string filename = "f" + std::to_string(x) + std::to_string(y) + ".kwj";
                auto path = ::test::testdata_dir() / "kwaj" / filename;
                if (!std::filesystem::exists(path)) continue;

                auto data = load_kwaj_file(path);
                REQUIRE_MESSAGE(data.has_value(), "Failed to load " << filename);

                auto header = kwaj_decompressor::parse_header(*data);
                REQUIRE_MESSAGE(header.has_value(), "Failed to parse header for " << filename);

                // Check filename if X > 0
                if (x > 0) {
                    CHECK_MESSAGE(header->filename == expected_filenames[x],
                        filename << ": expected filename '" << expected_filenames[x]
                        << "' but got '" << header->filename << "'");
                }

                // Check extension if Y > 0
                if (y > 0) {
                    CHECK_MESSAGE(header->extension == expected_extensions[y],
                        filename << ": expected extension '" << expected_extensions[y]
                        << "' but got '" << header->extension << "'");
                }

                // Verify extraction works
                auto result = decompress_kwaj(*data);
                CHECK_MESSAGE(result.has_value(), "Failed to extract " << filename);
            }
        }
    }
}
