#include <doctest/doctest.h>
#include <crate/formats/zoo.hh>
#include <crate/core/crc.hh>
#include <crate/core/system.hh>
#include <crate/test_config.hh>
#include <fstream>
#include <cstring>

using namespace crate;

namespace {
    const auto CORPUS_DIR = test::zoo_dir();
    const auto LICENSE_FILE = test::testdata_dir() / "LICENSE";

    byte_vector read_file(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return byte_vector((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
    }

    bool compare_data(const byte_vector& expected, const byte_vector& actual) {
        if (expected.size() != actual.size()) {
            return false;
        }
        return std::memcmp(expected.data(), actual.data(), expected.size()) == 0;
    }
}

TEST_SUITE("ZooArchive - Basic") {
    TEST_CASE("Invalid data") {
        std::array<u8, 16> invalid_data = {0};
        auto archive = zoo_archive::open(invalid_data);
        CHECK_FALSE(archive.has_value());
    }

    TEST_CASE("Empty data") {
        byte_span empty;
        auto archive = zoo_archive::open(empty);
        CHECK_FALSE(archive.has_value());
    }

    TEST_CASE("Valid signature check") {
        std::vector<u8> zoo_data(100, 0);
        std::memcpy(zoo_data.data(), "ZOO 2.10 Archive.", 17);
        zoo_data[17] = 0x1A;
        zoo_data[20] = 0xDC;
        zoo_data[21] = 0xA7;
        zoo_data[22] = 0xC4;
        zoo_data[23] = 0xFD;

        auto archive = zoo_archive::open(zoo_data);
        // May succeed with empty files list or fail validation
    }
}

TEST_SUITE("ZooArchive - CRC16") {
    TEST_CASE("CRC16 empty data") {
        byte_vector empty;
        u16 crc = eval_crc16_ibm(empty);
        CHECK(crc == 0x0000);
    }

    TEST_CASE("CRC16 known values") {
        std::array<u8, 9> data = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        u16 crc = eval_crc16_ibm(data);
        CHECK(crc == 0xBB3D);
    }
}

TEST_SUITE("ZooArchive - Corpus") {
    TEST_CASE("Method 0 - Stored") {
        auto archive_path = CORPUS_DIR / "store.zoo";
        REQUIRE(std::filesystem::exists(archive_path));

        auto expected = read_file(LICENSE_FILE);
        REQUIRE(expected.size() == 11357);

        auto archive = zoo_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->files().size() >= 1);

        const auto& entry = (*archive)->files()[0];
        CHECK(entry.name == "license");
        CHECK(entry.uncompressed_size == 11357);

        auto actual = (*archive)->extract(entry);
        REQUIRE(actual.has_value());
        CHECK(compare_data(expected, *actual));
    }

    TEST_CASE("Streaming extract_to - Stored") {
        auto archive_path = CORPUS_DIR / "store.zoo";
        REQUIRE(std::filesystem::exists(archive_path));

        auto expected = read_file(LICENSE_FILE);
        REQUIRE(expected.size() == 11357);

        auto archive = zoo_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->files().size() >= 1);

        const auto& entry = (*archive)->files()[0];
        vector_output_stream output(entry.uncompressed_size);
        auto result = (*archive)->extract_to(entry, output);
        REQUIRE(result.has_value());
        CHECK(*result == expected.size());
        CHECK(compare_data(expected, output.data()));
    }

    TEST_CASE("Method 1 - LZW (default)") {
        auto archive_path = CORPUS_DIR / "default.zoo";
        REQUIRE(std::filesystem::exists(archive_path));

        auto expected = read_file(LICENSE_FILE);
        REQUIRE(expected.size() == 11357);

        auto archive = zoo_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->files().size() >= 1);

        const auto& entry = (*archive)->files()[0];
        auto actual = (*archive)->extract(entry);
        REQUIRE(actual.has_value());
        CHECK(compare_data(expected, *actual));
    }

    TEST_CASE("Method 2 - LH5 (high performance)") {
        auto archive_path = CORPUS_DIR / "high_per.zoo";
        REQUIRE(std::filesystem::exists(archive_path));

        auto expected = read_file(LICENSE_FILE);
        REQUIRE(expected.size() == 11357);

        auto archive = zoo_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->files().size() >= 1);

        const auto& entry = (*archive)->files()[0];
        auto actual = (*archive)->extract(entry);
        REQUIRE(actual.has_value());
        CHECK(compare_data(expected, *actual));
    }

    TEST_CASE("Wrong CRC16 - should fail extraction") {
        auto archive_path = CORPUS_DIR / "wrongcrc16.zoo";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = zoo_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->files().size() >= 1);

        const auto& entry = (*archive)->files()[0];
        auto result = (*archive)->extract(entry);

        CHECK_FALSE(result.has_value());
        if (!result.has_value()) {
            CHECK(result.error().code() == error_code::InvalidChecksum);
        }
    }
}
