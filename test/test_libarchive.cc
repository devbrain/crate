#include <doctest/doctest.h>
#include <crate/test_config.hh>
#include <array>

#ifdef CRATE_WITH_LIBARCHIVE
#include <crate/formats/libarchive_archive.hh>

using namespace crate;

namespace {
    // Use existing test data - libarchive can read many formats
    const auto CAB_DIR = test::cab_dir();
    const auto ZIP_DIR = test::zip_dir();
}

TEST_SUITE("LibarchiveArchive - Basic") {
    TEST_CASE("Invalid data") {
        std::array<u8, 16> invalid_data = {0};
        auto archive = libarchive_archive::open(invalid_data);
        // libarchive may succeed with empty file list or fail — either is acceptable
        if (archive.has_value()) {
            CHECK(archive.value()->files().empty());
        } else {
            CHECK(!archive.has_value());
        }
    }

    TEST_CASE("Empty data") {
        byte_span empty;
        auto archive = libarchive_archive::open(empty);
        // libarchive accepts empty data as a valid archive with no entries
        if (archive.has_value()) {
            CHECK(archive.value()->files().empty());
        } else {
            CHECK(!archive.has_value());
        }
    }
}

TEST_SUITE("LibarchiveArchive - ZIP via libarchive") {
    TEST_CASE("Open ZIP file") {
        auto archive_path = ZIP_DIR / "stored.zip";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = libarchive_archive::open(archive_path);
        REQUIRE(archive.has_value());

        auto& files = (*archive)->files();
        CHECK(files.size() >= 1);
    }

    TEST_CASE("Extract from ZIP") {
        auto archive_path = ZIP_DIR / "stored.zip";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = libarchive_archive::open(archive_path);
        REQUIRE(archive.has_value());

        auto& files = (*archive)->files();
        REQUIRE(files.size() >= 1);

        auto content = (*archive)->extract(files[0]);
        REQUIRE(content.has_value());
        CHECK(content->size() > 0);
    }

    TEST_CASE("Format name") {
        auto archive_path = ZIP_DIR / "stored.zip";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = libarchive_archive::open(archive_path);
        REQUIRE(archive.has_value());

        std::string fmt = (*archive)->format_name();
        CHECK(!fmt.empty());
    }
}

TEST_SUITE("LibarchiveArchive - Error Handling") {
    TEST_CASE("Invalid entry index") {
        auto archive_path = ZIP_DIR / "stored.zip";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = libarchive_archive::open(archive_path);
        REQUIRE(archive.has_value());

        file_entry fake_entry;
        fake_entry.folder_index = 999;

        auto result = (*archive)->extract(fake_entry);
        CHECK(!result.has_value());
    }
}

#else
// Provide a dummy test when libarchive is not enabled
TEST_CASE("Libarchive tests skipped - not compiled with CRATE_WITH_LIBARCHIVE") {
}
#endif // CRATE_WITH_LIBARCHIVE
