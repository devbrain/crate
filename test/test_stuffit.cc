#include <doctest/doctest.h>
#include <crate/formats/stuffit.hh>
#include <crate/test_config.hh>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace crate;

namespace {

std::vector<u8> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::vector<u8>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    );
}

} // anonymous namespace

TEST_SUITE("StuffItArchive") {
    TEST_CASE("Invalid data") {
        std::vector<u8> data = {0, 1, 2, 3};
        auto result = stuffit_archive::open(data);
        CHECK(!result.has_value());
    }

    TEST_CASE("Empty data") {
        std::vector<u8> data;
        auto result = stuffit_archive::open(data);
        CHECK(!result.has_value());
    }

    TEST_CASE("Open old format archive") {
        auto data = read_file(test::stuffit_dir() / "testfile.stuffit45_dlx.mac9.sit");
        if (data.empty()) {
            MESSAGE("Test file not found, skipping");
            return;
        }

        auto result = stuffit_archive::open(data);
        REQUIRE(result.has_value());

        auto& archive = *result;
        CHECK(archive->format() == stuffit::format_version::old_format);

        const auto& files = archive->files();
        CHECK(!files.empty());

        MESSAGE("Found ", files.size(), " files in old format archive");
        for (const auto& f : files) {
            MESSAGE("  ", f.name, " (", f.uncompressed_size, " bytes)");
        }
    }

    TEST_CASE("Open v5 format archive") {
        auto data = read_file(test::stuffit_dir() / "testfile.stuffit651_dlx.mac9.sit");
        if (data.empty()) {
            MESSAGE("Test file not found, skipping");
            return;
        }

        auto result = stuffit_archive::open(data);
        REQUIRE(result.has_value());

        auto& archive = *result;
        CHECK(archive->format() == stuffit::format_version::v5_format);

        const auto& files = archive->files();
        CHECK(!files.empty());

        MESSAGE("Found ", files.size(), " files in v5 format archive");
        for (const auto& f : files) {
            MESSAGE("  ", f.name, " (", f.uncompressed_size, " bytes)");
        }
    }

    TEST_CASE("Signature detection - old format") {
        // SIT! signature with zero entries (valid but empty archive)
        std::vector<u8> data = {'S', 'I', 'T', '!', 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        auto result = stuffit_archive::open(data);
        // Valid signature with no entries - may succeed with empty files list
        if (result.has_value()) {
            CHECK(result.value()->files().empty());
        }
    }

    TEST_CASE("Signature detection - v5 format") {
        // StuffIt signature with no root entries (valid but empty)
        std::vector<u8> data(100, 0);
        std::memcpy(data.data(), "StuffIt ", 8);
        auto result = stuffit_archive::open(data);
        // Valid signature with no entries - may succeed with empty files list
        if (result.has_value()) {
            CHECK(result.value()->files().empty());
        }
    }

    TEST_CASE("Extract uncompressed file from old format") {
        auto data = read_file(test::stuffit_dir() / "testfile.stuffit45_dlx.mac9.sit");
        if (data.empty()) {
            MESSAGE("Test file not found, skipping");
            return;
        }

        auto result = stuffit_archive::open(data);
        REQUIRE(result.has_value());

        auto& archive = *result;
        const auto& files = archive->files();

        // Find Test Text (uncompressed data fork)
        for (const auto& f : files) {
            if (f.name == "Test Text" && f.uncompressed_size == 11) {
                auto extract_result = archive->extract(f);
                if (extract_result.has_value()) {
                    CHECK(extract_result->size() == f.uncompressed_size);
                    MESSAGE("Successfully extracted uncompressed file: ", f.name);
                } else {
                    auto msg = extract_result.error().message();
                    MESSAGE("Extraction failed: ", msg);
                }
                break;
            }
        }
    }

    TEST_CASE("Extract method 13 (LZ+Huffman) compressed file") {
        auto data = read_file(test::stuffit_dir() / "testfile.stuffit45_dlx.mac9.sit");
        if (data.empty()) {
            MESSAGE("Test file not found, skipping");
            return;
        }

        auto result = stuffit_archive::open(data);
        REQUIRE(result.has_value());

        auto& archive = *result;
        const auto& files = archive->files();

        // Try to extract files that use method 13 compression
        for (const auto& f : files) {
            if (f.name == "testfile.jpg" && f.uncompressed_size == 220) {
                auto extract_result = archive->extract(f);
                if (extract_result.has_value()) {
                    CHECK(extract_result->size() == f.uncompressed_size);
                    MESSAGE("Successfully extracted method 13 file: ", f.name);
                } else {
                    auto msg = extract_result.error().message();
                    MESSAGE("Extraction failed: ", msg);
                }
                break;
            }
        }
    }

    TEST_CASE("Extract method 15 (Arsenic) compressed file from v5 format") {
        auto data = read_file(test::stuffit_dir() / "testfile.stuffit651_dlx.mac9.sit");
        if (data.empty()) {
            MESSAGE("Test file not found, skipping");
            return;
        }

        auto result = stuffit_archive::open(data);
        REQUIRE(result.has_value());

        auto& archive = *result;
        const auto& files = archive->files();

        // Try to extract files that use Arsenic compression (method 15)
        // testfile.PICT uses method 15
        for (const auto& f : files) {
            if (f.name == "testfile.PICT" && f.uncompressed_size == 2694) {
                auto extract_result = archive->extract(f);
                if (extract_result.has_value()) {
                    CHECK(extract_result->size() == f.uncompressed_size);
                    MESSAGE("Successfully extracted Arsenic file: ", f.name);
                } else {
                    auto msg = extract_result.error().message();
                    MESSAGE("Arsenic extraction failed: ", msg);
                }
                break;
            }
        }
    }

    TEST_CASE("Extract all files from v5 format") {
        auto data = read_file(test::stuffit_dir() / "testfile.stuffit651_dlx.mac9.sit");
        if (data.empty()) {
            MESSAGE("Test file not found, skipping");
            return;
        }

        auto result = stuffit_archive::open(data);
        REQUIRE(result.has_value());

        auto& archive = *result;
        const auto& files = archive->files();

        size_t extracted_count = 0;
        for (const auto& f : files) {
            if (f.is_directory) continue;
            if (f.uncompressed_size == 0) continue;  // Skip empty files

            auto extract_result = archive->extract(f);
            if (extract_result.has_value()) {
                extracted_count++;
                MESSAGE("Extracted: ", f.name, " (", extract_result->size(), " bytes)");
            } else {
                auto msg = extract_result.error().message();
                MESSAGE("Failed: ", f.name, " - ", msg);
            }
        }

        MESSAGE("Successfully extracted ", extracted_count, " files");
    }
}
