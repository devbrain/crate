#include <doctest/doctest.h>
#include <crate/formats/arj.hh>
#include <crate/core/crc.hh>
#include <crate/core/system.hh>
#include <crate/compression/arj_lz.hh>
#include <crate/compression/lzh.hh>
#include <crate/test_config.hh>
#include "md5.h"
#include <array>
#include <cstring>
#include <fstream>
#include <filesystem>

using namespace crate;

TEST_SUITE("ArjArchive - Basic") {
    TEST_CASE("Invalid signature") {
        std::array<u8, 16> invalid_data = {0};
        auto archive = arj_archive::open(invalid_data);
        CHECK_FALSE(archive.has_value());
        CHECK(archive.error().code() == error_code::InvalidSignature);
    }

    TEST_CASE("Truncated header") {
        std::array<u8, 4> truncated = {0x60, 0xEA, 0x00, 0x00};
        auto archive = arj_archive::open(truncated);
        CHECK_FALSE(archive.has_value());
        CHECK((archive.error().code() == error_code::InvalidHeader ||
               archive.error().code() == error_code::TruncatedArchive));
    }

    TEST_CASE("Minimal valid signature check") {
        std::vector<u8> arj_data(64, 0);
        arj_data[0] = 0x60;
        arj_data[1] = 0xEA;
        arj_data[2] = 32;
        arj_data[3] = 0;
        arj_data[4] = 30;
        arj_data[5] = 2;
        arj_data[6] = 1;
        arj_data[7] = 0;
        arj_data[8] = 0;
        arj_data[9] = 0;
        arj_data[10] = 2;

        auto archive = arj_archive::open(arj_data);
        // May succeed or fail depending on CRC, but shouldn't crash
    }
}

TEST_SUITE("ArjArchive - CRC32") {
    TEST_CASE("CRC32 empty data") {
        byte_vector empty;
        u32 crc = eval_crc_32(empty);
        CHECK(crc == 0x00000000);
    }

    TEST_CASE("CRC32 known values") {
        std::array<u8, 9> data = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        u32 crc = eval_crc_32(data);
        CHECK(crc == 0xCBF43926);
    }

    TEST_CASE("CRC32 incremental") {
        std::array<u8, 9> data = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};

        crc_32 crc;
        for (u8 b : data) {
            crc.update(b);
        }
        CHECK(crc.finalize() == 0xCBF43926);
    }
}

TEST_SUITE("ArjArchive - Method 4 Decompressor") {
    TEST_CASE("Empty input") {
        arj_method4_decompressor decomp;
        std::array<u8, 0> input{};
        std::array<u8, 100> output{};

        auto result = decomp.decompress(input, output);
        CHECK(result.has_value());
        CHECK(*result == 0);
    }

    TEST_CASE("Literal only") {
        arj_method4_decompressor decomp;
        std::array<u8, 2> input = {0x20, 0x80};
        std::array<u8, 10> output{};

        auto result = decomp.decompress(input, output);
        CHECK(result.has_value());
    }
}

TEST_SUITE("ArjArchive - LZH Decompressor") {
    TEST_CASE("LH6 format initialization") {
        lzh_decompressor decomp(lzh_format::LH6);
        std::array<u8, 0> input{};
        std::array<u8, 100> output{};
        auto result = decomp.decompress(input, output);
        CHECK(result.has_value());
        CHECK(*result == 0);
    }

    TEST_CASE("LH7 format initialization") {
        lzh_decompressor decomp(lzh_format::LH7);
        std::array<u8, 0> input{};
        std::array<u8, 100> output{};
        auto result = decomp.decompress(input, output);
        CHECK(result.has_value());
        CHECK(*result == 0);
    }

    TEST_CASE("Empty input") {
        lzh_decompressor decomp(lzh_format::LH6);
        std::array<u8, 0> input{};
        std::array<u8, 100> output{};

        auto result = decomp.decompress(input, output);
        CHECK(result.has_value());
        CHECK(*result == 0);
    }
}

TEST_SUITE("ArjArchive - Path Sanitization") {
    TEST_CASE("Paths with backslashes are normalized") {
        auto archive_path = test::arj_dir() / "stored.arj";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = arj_archive::open(archive_path);
        REQUIRE(archive.has_value());

        for (const auto& f : (*archive)->files()) {
            CHECK(f.name.find('\\') == std::string::npos);
        }
    }
}


TEST_SUITE("ArjArchive - Corpus") {
    const auto CORPUS_DIR = test::arj_dir();

    // Reference file for all ARJ corpus tests: testdata/LICENSE
    constexpr size_t LICENSE_SIZE = 11357;
    const char* LICENSE_MD5 = "86d3f3a95c324c9479bd8986968f4327";

    std::string compute_md5(const byte_vector& data) {
        MD5_CTX ctx;
        MD5_Init(&ctx);
        unsigned long sz = data.size();
        MD5_Update(&ctx, data.data(), sz);
        unsigned char digest[MD5_DIGEST_LENGTH];
        MD5_Final(digest, &ctx);
        char hex[33];
        for (int i = 0; i < 16; i++)
            snprintf(hex + i * 2, 3, "%02x", digest[i]);
        return hex;
    }

    byte_vector read_reference_file() {
        auto ref_path = test::testdata_dir() / "LICENSE";
        std::ifstream file(ref_path, std::ios::binary);
        return byte_vector((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
    }

    TEST_CASE("Method 0 - Stored") {
        auto archive_path = CORPUS_DIR / "stored.arj";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = arj_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->files().size() == 1);

        const auto& entry = (*archive)->files()[0];
        CHECK(entry.name == "LICENSE");
        CHECK(entry.uncompressed_size == LICENSE_SIZE);

        auto actual = (*archive)->extract(entry);
        REQUIRE(actual.has_value());
        CHECK(actual->size() == LICENSE_SIZE);
        CHECK(compute_md5(*actual) == LICENSE_MD5);

        auto ref = read_reference_file();
        REQUIRE(ref.size() == LICENSE_SIZE);
        CHECK(std::memcmp(actual->data(), ref.data(), ref.size()) == 0);
    }

    TEST_CASE("Method 1 - LZH") {
        auto archive_path = CORPUS_DIR / "method1.arj";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = arj_archive::open(archive_path);
        REQUIRE(archive.has_value());

        const auto& entry = (*archive)->files()[0];
        auto actual = (*archive)->extract(entry);
        REQUIRE(actual.has_value());
        CHECK(actual->size() == LICENSE_SIZE);
        CHECK(compute_md5(*actual) == LICENSE_MD5);
    }

    TEST_CASE("Streaming extract_to - LZH") {
        auto archive_path = CORPUS_DIR / "method1.arj";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = arj_archive::open(archive_path);
        REQUIRE(archive.has_value());

        const auto& entry = (*archive)->files()[0];
        auto expected = (*archive)->extract(entry);
        REQUIRE(expected.has_value());

        vector_output_stream output;
        auto result = (*archive)->extract_to(entry, output);
        REQUIRE(result.has_value());
        CHECK(*result == expected->size());
        CHECK(std::memcmp(expected->data(), output.data().data(), expected->size()) == 0);
    }

    TEST_CASE("Method 2 - LZH") {
        auto archive_path = CORPUS_DIR / "method2.arj";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = arj_archive::open(archive_path);
        REQUIRE(archive.has_value());

        const auto& entry = (*archive)->files()[0];
        auto actual = (*archive)->extract(entry);
        REQUIRE(actual.has_value());
        CHECK(actual->size() == LICENSE_SIZE);
        CHECK(compute_md5(*actual) == LICENSE_MD5);
    }

    TEST_CASE("Method 3 - LZH") {
        auto archive_path = CORPUS_DIR / "method3.arj";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = arj_archive::open(archive_path);
        REQUIRE(archive.has_value());

        const auto& entry = (*archive)->files()[0];
        auto actual = (*archive)->extract(entry);
        REQUIRE(actual.has_value());
        CHECK(actual->size() == LICENSE_SIZE);
        CHECK(compute_md5(*actual) == LICENSE_MD5);
    }

    TEST_CASE("Method 4 - LZ77") {
        auto archive_path = CORPUS_DIR / "method4.arj";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = arj_archive::open(archive_path);
        REQUIRE(archive.has_value());

        const auto& entry = (*archive)->files()[0];
        auto actual = (*archive)->extract(entry);
        REQUIRE(actual.has_value());
        CHECK(actual->size() == LICENSE_SIZE);
        CHECK(compute_md5(*actual) == LICENSE_MD5);
    }

    TEST_CASE("Wrong CRC32 - should fail extraction") {
        auto archive_path = CORPUS_DIR / "wrongcrc32.arj";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = arj_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->files().size() == 1);

        const auto& entry = (*archive)->files()[0];
        auto result = (*archive)->extract(entry);

        CHECK_FALSE(result.has_value());
        if (!result.has_value()) {
            CHECK(result.error().code() == error_code::InvalidChecksum);
        }
    }
}
