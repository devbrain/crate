#include <doctest/doctest.h>
#include <crate/formats/ace.hh>
#include <crate/test_config.hh>
#include <fstream>
#include <cstring>

using namespace crate;

namespace {
    const auto CORPUS_DIR = test::ace_dir();
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

TEST_SUITE("AceArchive - Basic") {
    TEST_CASE("Invalid data") {
        std::array<u8, 16> invalid_data = {0};
        auto archive = ace_archive::open(invalid_data);
        CHECK_FALSE(archive.has_value());
    }

    TEST_CASE("Empty data") {
        byte_span empty;
        auto archive = ace_archive::open(empty);
        CHECK_FALSE(archive.has_value());
    }

    TEST_CASE("Valid signature only") {
        std::vector<u8> ace_data = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            '*', '*', 'A', 'C', 'E', '*', '*'
        };
        auto archive = ace_archive::open(ace_data);
        // May fail due to incomplete header
    }
}

TEST_SUITE("AceArchive - Corpus") {
    TEST_CASE("license1.ace - compressed file") {
        auto archive_path = CORPUS_DIR / "license1.ace";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = ace_archive::open(archive_path);
        REQUIRE(archive.has_value());

        auto& files = (*archive)->files();
        auto expected = read_file(LICENSE_FILE);

        for (const auto& entry : files) {
            auto actual = (*archive)->extract(entry);
            REQUIRE(actual.has_value());
            if (entry.name == "LICENSE" && std::filesystem::exists(LICENSE_FILE)) {
                CHECK(compare_data(expected, *actual));
            }
        }
    }

    TEST_CASE("license2.ace - compressed file") {
        auto archive_path = CORPUS_DIR / "license2.ace";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = ace_archive::open(archive_path);
        REQUIRE(archive.has_value());

        auto& files = (*archive)->files();
        auto expected = read_file(LICENSE_FILE);

        for (const auto& entry : files) {
            auto actual = (*archive)->extract(entry);
            REQUIRE(actual.has_value());
            if (entry.name == "LICENSE" && std::filesystem::exists(LICENSE_FILE)) {
                CHECK(compare_data(expected, *actual));
            }
        }
    }
}
