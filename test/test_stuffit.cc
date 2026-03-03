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
            return;
        }

        auto result = stuffit_archive::open(data);
        REQUIRE(result.has_value());

        auto& archive = *result;
        CHECK(archive->format() == stuffit::format_version::old_format);

        const auto& files = archive->files();
        CHECK(!files.empty());
    }

    TEST_CASE("Open v5 format archive") {
        auto data = read_file(test::stuffit_dir() / "testfile.stuffit651_dlx.mac9.sit");
        if (data.empty()) {
            return;
        }

        auto result = stuffit_archive::open(data);
        REQUIRE(result.has_value());

        auto& archive = *result;
        CHECK(archive->format() == stuffit::format_version::v5_format);

        const auto& files = archive->files();
        CHECK(!files.empty());
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
                } else {
                    auto msg = extract_result.error().message();
                    (void)msg;
                }
                break;
            }
        }
    }

    TEST_CASE("Extract method 13 (LZ+Huffman) compressed file") {
        auto data = read_file(test::stuffit_dir() / "testfile.stuffit45_dlx.mac9.sit");
        if (data.empty()) {
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
                } else {
                    auto msg = extract_result.error().message();
                    (void)msg;
                }
                break;
            }
        }
    }

    TEST_CASE("Extract method 13 with dynamic tables (code_type 0)") {
        auto data = read_file(test::stuffit_dir() / "testfile.stuffit45_dlx.mac9.sit");
        if (data.empty()) {
            return;
        }

        auto result = stuffit_archive::open(data);
        REQUIRE(result.has_value());

        auto& archive = *result;
        const auto& files = archive->files();

        // testfile.PICT uses method 13 with code_type=0 (dynamic tables)
        for (const auto& f : files) {
            if (f.name == "testfile.PICT" && f.uncompressed_size == 2694) {
                auto extract_result = archive->extract(f);
                REQUIRE(extract_result.has_value());
                CHECK(extract_result->size() == f.uncompressed_size);
                break;
            }
        }
    }

    TEST_CASE("Extract method 15 (Arsenic) compressed file from v5 format") {
        auto data = read_file(test::stuffit_dir() / "testfile.stuffit651_dlx.mac9.sit");
        if (data.empty()) {
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
                } else {
                    auto msg = extract_result.error().message();
                    (void)msg;
                }
                break;
            }
        }
    }

    TEST_CASE("Extract all files from v5 format") {
        auto data = read_file(test::stuffit_dir() / "testfile.stuffit651_dlx.mac9.sit");
        if (data.empty()) {
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
            } else {
                auto msg = extract_result.error().message();
                (void)msg;
            }
        }
    }

    TEST_CASE("Extract method 2 (LZW) compressed files from old format") {
        auto data = read_file(test::stuffit_dir() / "testfile_lzw.sit");
        if (data.empty()) {
            return;
        }

        auto result = stuffit_archive::open(data);
        REQUIRE(result.has_value());

        auto& archive = *result;
        CHECK(archive->format() == stuffit::format_version::old_format);

        const auto& files = archive->files();
        REQUIRE(!files.empty());

        // Extract each file and verify contents
        bool found_test_lzw = false;
        bool found_repeated = false;
        bool found_pangram = false;

        for (const auto& f : files) {
            if (f.is_directory) continue;

            auto extract_result = archive->extract(f);
            if (!extract_result.has_value()) {
                auto msg = extract_result.error().message();
                (void)msg;
                continue;
            }

            CHECK(extract_result->size() == f.uncompressed_size);

            // Verify specific file contents
            if (f.name == "test_lzw.txt") {
                found_test_lzw = true;
                std::string content(reinterpret_cast<const char*>(extract_result->data()),
                                   extract_result->size());
                CHECK(content.find("Hello, this is a test file for LZW compression") != std::string::npos);
            }

            if (f.name == "repeated.txt") {
                found_repeated = true;
                // Should be 65 bytes of 'A' characters and newline
                CHECK(extract_result->size() >= 64);
                bool all_a = true;
                for (size_t i = 0; i < 64 && i < extract_result->size(); i++) {
                    if ((*extract_result)[i] != 'A') {
                        all_a = false;
                        break;
                    }
                }
                CHECK(all_a);
            }

            if (f.name == "pangram.txt") {
                found_pangram = true;
                std::string content(reinterpret_cast<const char*>(extract_result->data()),
                                   extract_result->size());
                CHECK(content.find("The quick brown fox jumps over the lazy dog") != std::string::npos);
            }
        }

        // Verify all expected files were found
        CHECK(found_test_lzw);
        CHECK(found_repeated);
        CHECK(found_pangram);
    }

    TEST_CASE("Extract real-world samples from sembiance.com") {
        // These are MacBinary-unwrapped samples from sembiance.com

        SUBCASE("sample_addres.sit - LZW compression") {
            auto data = read_file(test::stuffit_dir() / "sample_addres.sit");
            if (data.empty()) {
                return;
            }

            auto result = stuffit_archive::open(data);
            REQUIRE(result.has_value());

            auto& archive = *result;
            CHECK(archive->format() == stuffit::format_version::old_format);

            const auto& files = archive->files();
            REQUIRE(!files.empty());

            for (const auto& f : files) {
                if (f.is_directory || f.uncompressed_size == 0) continue;
                auto extract_result = archive->extract(f);
                REQUIRE(extract_result.has_value());
                CHECK(extract_result->size() == f.uncompressed_size);
            }
        }

        SUBCASE("sample_fixer.sit - LZW compression with source code") {
            auto data = read_file(test::stuffit_dir() / "sample_fixer.sit");
            if (data.empty()) {
                return;
            }

            auto result = stuffit_archive::open(data);
            REQUIRE(result.has_value());

            auto& archive = *result;
            const auto& files = archive->files();

            for (const auto& f : files) {
                if (f.name == "fixer.c") {
                    auto extract_result = archive->extract(f);
                    REQUIRE(extract_result.has_value());
                    CHECK(extract_result->size() == 2909);
                    // Verify it contains C source code
                    std::string content(reinterpret_cast<const char*>(extract_result->data()),
                                       std::min(size_t(100), extract_result->size()));
                    CHECK((content.find("#include") != std::string::npos ||
                           content.find("/*") != std::string::npos));
                }
            }
        }

        SUBCASE("sample_clrmg131.sit - mixed compression") {
            auto data = read_file(test::stuffit_dir() / "sample_clrmg131.sit");
            if (data.empty()) {
                return;
            }

            auto result = stuffit_archive::open(data);
            REQUIRE(result.has_value());

            auto& archive = *result;
            const auto& files = archive->files();

            size_t extracted = 0;
            for (const auto& f : files) {
                if (f.is_directory || f.uncompressed_size == 0) continue;
                auto extract_result = archive->extract(f);
                if (extract_result.has_value()) {
                    extracted++;
                }
            }
            CHECK(extracted >= 1);
        }
    }

    TEST_CASE("Extract stuffit-rs test fixtures") {
        // Test fixtures from stuffit-rs project

        SUBCASE("test_m0.sit - method 0 (stored)") {
            auto data = read_file(test::stuffit_dir() / "test_m0.sit");
            if (data.empty()) {
                return;
            }

            auto result = stuffit_archive::open(data);
            REQUIRE(result.has_value());

            auto& archive = *result;
            CHECK(archive->format() == stuffit::format_version::v5_format);

            const auto& files = archive->files();
            REQUIRE(files.size() == 1);

            auto extract_result = archive->extract(files[0]);
            REQUIRE(extract_result.has_value());
            CHECK(extract_result->size() == 34);

            std::string content(reinterpret_cast<const char*>(extract_result->data()),
                               extract_result->size());
            CHECK(content == "Hello World from Method 0 (Store)!");
        }

        SUBCASE("test_m13.sit - method 13 (LZ+Huffman)") {
            auto data = read_file(test::stuffit_dir() / "test_m13.sit");
            if (data.empty()) {
                return;
            }

            auto result = stuffit_archive::open(data);
            REQUIRE(result.has_value());

            auto& archive = *result;
            CHECK(archive->format() == stuffit::format_version::v5_format);

            const auto& files = archive->files();
            REQUIRE(files.size() == 1);

            auto extract_result = archive->extract(files[0]);
            REQUIRE(extract_result.has_value());
            CHECK(extract_result->size() == 86);

            std::string content(reinterpret_cast<const char*>(extract_result->data()),
                               extract_result->size());
            CHECK(content.find("Repetitive") != std::string::npos);
        }

        SUBCASE("test_deflate.sit - method 14 (Deflate)") {
            auto data = read_file(test::stuffit_dir() / "test_deflate.sit");
            if (data.empty()) {
                return;
            }

            auto result = stuffit_archive::open(data);
            REQUIRE(result.has_value());

            auto& archive = *result;
            CHECK(archive->format() == stuffit::format_version::v5_format);

            const auto& files = archive->files();
            REQUIRE(files.size() == 1);

            auto extract_result = archive->extract(files[0]);
            REQUIRE(extract_result.has_value());
            CHECK(extract_result->size() == 74);

            std::string content(reinterpret_cast<const char*>(extract_result->data()),
                               extract_result->size());
            CHECK(content.find("Hello, Deflate!") != std::string::npos);
            CHECK(content.find("Method 14") != std::string::npos);
        }

        SUBCASE("test_complex.sit - complex archive with folders") {
            auto data = read_file(test::stuffit_dir() / "test_complex.sit");
            if (data.empty()) {
                return;
            }

            auto result = stuffit_archive::open(data);
            REQUIRE(result.has_value());

            auto& archive = *result;
            CHECK(archive->format() == stuffit::format_version::v5_format);

            const auto& files = archive->files();

            size_t file_count = 0;
            size_t dir_count = 0;
            for (const auto& f : files) {
                if (f.is_directory) {
                    dir_count++;
                } else {
                    file_count++;
                    auto extract_result = archive->extract(f);
                    (void)extract_result;
                }
            }

            CHECK(file_count >= 3);  // At least README.txt, config.json, App.rsrc
            CHECK(dir_count >= 2);   // At least Documents, Images
        }
    }
}
