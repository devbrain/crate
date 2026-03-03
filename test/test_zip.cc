#include <doctest/doctest.h>
#include <crate/formats/zip.hh>
#include <crate/test_config.hh>
#include <array>

using namespace crate;

namespace {
    const auto ZIP_DIR = test::zip_dir();
}

TEST_SUITE("ZipArchive - Basic") {
    TEST_CASE("Invalid data") {
        std::array<u8, 16> invalid_data = {0};
        auto archive = zip_archive::open(invalid_data);
        CHECK(!archive.has_value());
    }

    TEST_CASE("Empty data") {
        byte_span empty;
        auto archive = zip_archive::open(empty);
        CHECK(!archive.has_value());
    }

    TEST_CASE("Too small for ZIP") {
        std::array<u8, 10> small = {0};
        auto archive = zip_archive::open(small);
        CHECK(!archive.has_value());
    }
}

TEST_SUITE("ZipArchive - Stored") {
    TEST_CASE("Open stored ZIP") {
        auto archive_path = ZIP_DIR / "stored.zip";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = zip_archive::open(archive_path);
        REQUIRE(archive.has_value());

        auto& files = (*archive)->files();
        REQUIRE(files.size() == 1);
        CHECK(files[0].name == "hello.txt");
        CHECK(files[0].uncompressed_size == 14);
        CHECK(!files[0].is_directory);
        CHECK(!files[0].is_encrypted);
    }

    TEST_CASE("Extract stored file") {
        auto archive_path = ZIP_DIR / "stored.zip";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = zip_archive::open(archive_path);
        REQUIRE(archive.has_value());

        auto& files = (*archive)->files();
        REQUIRE(files.size() >= 1);

        auto content = (*archive)->extract(files[0]);
        REQUIRE(content.has_value());
        CHECK(content->size() == 14);

        std::string text(content->begin(), content->end());
        CHECK(text == "Hello, World!\n");
    }
}

TEST_SUITE("ZipArchive - Deflated") {
    TEST_CASE("Open deflated ZIP") {
        auto archive_path = ZIP_DIR / "deflated.zip";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = zip_archive::open(archive_path);
        REQUIRE(archive.has_value());

        auto& files = (*archive)->files();
        REQUIRE(files.size() == 1);
        CHECK(files[0].name == "test.txt");
    }

    TEST_CASE("Extract deflated file") {
        auto archive_path = ZIP_DIR / "deflated.zip";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = zip_archive::open(archive_path);
        REQUIRE(archive.has_value());

        auto& files = (*archive)->files();
        REQUIRE(files.size() >= 1);

        auto content = (*archive)->extract(files[0]);
        REQUIRE(content.has_value());

        std::string text(content->begin(), content->end());
        CHECK(text == "This is a test file for ZIP archive testing.\n");
    }
}

TEST_SUITE("ZipArchive - Multiple Files") {
    TEST_CASE("Open ZIP with multiple files") {
        auto archive_path = ZIP_DIR / "multiple.zip";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = zip_archive::open(archive_path);
        REQUIRE(archive.has_value());

        auto& files = (*archive)->files();
        CHECK(files.size() == 2);
    }

    TEST_CASE("Extract all files") {
        auto archive_path = ZIP_DIR / "multiple.zip";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = zip_archive::open(archive_path);
        REQUIRE(archive.has_value());

        for (const auto& entry : (*archive)->files()) {
            auto content = (*archive)->extract(entry);
            REQUIRE(content.has_value());
            CHECK(content->size() == entry.uncompressed_size);
        }
    }
}

TEST_SUITE("ZipArchive - Empty File") {
    TEST_CASE("Extract empty file") {
        auto archive_path = ZIP_DIR / "with_empty.zip";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = zip_archive::open(archive_path);
        REQUIRE(archive.has_value());

        auto& files = (*archive)->files();
        REQUIRE(files.size() >= 1);

        auto content = (*archive)->extract(files[0]);
        REQUIRE(content.has_value());
        CHECK(content->size() == 0);
    }
}

TEST_SUITE("ZipArchive - Error Handling") {
    TEST_CASE("Invalid entry index") {
        auto archive_path = ZIP_DIR / "stored.zip";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = zip_archive::open(archive_path);
        REQUIRE(archive.has_value());

        // Create a fake entry with invalid index
        file_entry fake_entry;
        fake_entry.folder_index = 999;

        auto result = (*archive)->extract(fake_entry);
        CHECK(!result.has_value());
    }
}
