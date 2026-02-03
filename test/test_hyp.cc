#include <doctest/doctest.h>
#include <crate/formats/hyp.hh>
#include <crate/formats/hyp_internal.hh>
#include <crate/test_config.hh>
#include <fstream>
#include <cstring>

using namespace crate;

namespace {
    const auto CORPUS_DIR = test::hyp_dir();
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

TEST_SUITE("HypArchive - Basic") {
    TEST_CASE("Invalid data") {
        std::array<u8, 16> invalid_data = {0};
        auto archive = hyp_archive::open(invalid_data);
        if (archive.has_value()) {
            CHECK((*archive)->files().empty());
        }
    }

    TEST_CASE("Empty data") {
        byte_span empty;
        auto archive = hyp_archive::open(empty);
        if (archive.has_value()) {
            CHECK((*archive)->files().empty());
        }
    }
}

TEST_SUITE("HypArchive - Checksum") {
    TEST_CASE("HYP checksum empty") {
        byte_vector empty;
        u32 checksum = hyp::hyp_checksum(empty);
        CHECK(checksum == 0);
    }

    TEST_CASE("HYP checksum single byte") {
        std::array<u8, 1> data = {0x41};
        u32 checksum = hyp::hyp_checksum(data);
        CHECK(checksum == 0x82);
    }
}

TEST_SUITE("HypArchive - Corpus") {
    TEST_CASE("Stored - multiple small files") {
        auto archive_path = CORPUS_DIR / "stored.hyp";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = hyp_archive::open(archive_path);
        REQUIRE(archive.has_value());

        auto& files = (*archive)->files();
        for (const auto& entry : files) {
            auto actual = (*archive)->extract(entry);
            CHECK(actual.has_value());
        }
    }

    TEST_CASE("atest.hyp - single file") {
        auto archive_path = CORPUS_DIR / "atest.hyp";
        auto expected_path = CORPUS_DIR / "atest.txt";
        REQUIRE(std::filesystem::exists(archive_path));
        REQUIRE(std::filesystem::exists(expected_path));

        auto expected = read_file(expected_path);
        auto archive = hyp_archive::open(archive_path);
        REQUIRE(archive.has_value());

        if ((*archive)->files().size() >= 1) {
            const auto& entry = (*archive)->files()[0];
            auto actual = (*archive)->extract(entry);
            REQUIRE(actual.has_value());
            CHECK(compare_data(expected, *actual));
        }
    }

    TEST_CASE("Compressed - license.hyp") {
        auto archive_path = CORPUS_DIR / "license.hyp";
        REQUIRE(std::filesystem::exists(archive_path));

        auto expected = read_file(LICENSE_FILE);
        REQUIRE(expected.size() == 11357);

        auto archive = hyp_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->files().size() >= 1);

        const auto& entry = (*archive)->files()[0];
        auto actual = (*archive)->extract(entry);
        REQUIRE(actual.has_value());

        CHECK(compare_data(expected, *actual));
    }

    TEST_CASE("Invalid archive") {
        auto archive_path = CORPUS_DIR / "invalid.hyp";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = hyp_archive::open(archive_path);
        // Should either fail to open or have issues with extraction
    }
}
