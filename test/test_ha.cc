#include <doctest/doctest.h>
#include <crate/formats/ha.hh>
#include <crate/test_config.hh>
#include <fstream>
#include <cstring>

using namespace crate;

namespace {
    const auto CORPUS_DIR = test::ha_dir();
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

TEST_SUITE("HaArchive - Basic") {
    TEST_CASE("Invalid data") {
        std::array<u8, 16> invalid_data = {0};
        auto archive = ha_archive::open(invalid_data);
        CHECK_FALSE(archive.has_value());
    }

    TEST_CASE("Empty data") {
        byte_span empty;
        auto archive = ha_archive::open(empty);
        CHECK_FALSE(archive.has_value());
    }

    TEST_CASE("Valid signature") {
        std::vector<u8> ha_data = {'H', 'A', 0, 0};
        auto archive = ha_archive::open(ha_data);
        CHECK(archive.has_value());
        CHECK((*archive)->files().empty());
    }
}

TEST_SUITE("HaArchive - Corpus") {
    TEST_CASE("Method 0 - CPY (Stored)") {
        auto archive_path = CORPUS_DIR / "copy.ha";
        REQUIRE(std::filesystem::exists(archive_path));

        auto expected = read_file(LICENSE_FILE);
        REQUIRE(expected.size() == 11357);

        auto archive = ha_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->files().size() >= 1);

        const auto& entry = (*archive)->files()[0];
        auto actual = (*archive)->extract(entry);
        REQUIRE(actual.has_value());
        CHECK(compare_data(expected, *actual));
    }

    TEST_CASE("Method 1 - ASC (LZ77+Arithmetic)") {
        auto archive_path = CORPUS_DIR / "asc.ha";
        REQUIRE(std::filesystem::exists(archive_path));

        auto expected = read_file(LICENSE_FILE);
        REQUIRE(expected.size() == 11357);

        auto archive = ha_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->files().size() >= 1);

        const auto& entry = (*archive)->files()[0];
        auto actual = (*archive)->extract(entry);
        REQUIRE(actual.has_value());
        CHECK(compare_data(expected, *actual));
    }

    TEST_CASE("Method 2 - HSC (PPM+Arithmetic)") {
        auto archive_path = CORPUS_DIR / "hsc.ha";
        REQUIRE(std::filesystem::exists(archive_path));

        auto expected = read_file(LICENSE_FILE);
        REQUIRE(expected.size() == 11357);

        auto archive = ha_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->files().size() >= 1);

        const auto& entry = (*archive)->files()[0];
        auto actual = (*archive)->extract(entry);
        REQUIRE(actual.has_value());
        CHECK(compare_data(expected, *actual));
    }
}
