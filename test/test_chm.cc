#include <doctest/doctest.h>
#include <crate/formats/chm.hh>
#include <crate/test_config.hh>
#include <array>

using namespace crate;

TEST_SUITE("ChmArchive - Basic") {
    TEST_CASE("Invalid signature") {
        std::array<u8, 16> invalid_data = {0};
        auto archive = chm_archive::open(invalid_data);
        CHECK_FALSE(archive.has_value());
        CHECK(archive.error().code() == error_code::InvalidSignature);
    }

    TEST_CASE("Truncated header") {
        // Just the signature
        std::array<u8, 4> truncated = {'I', 'T', 'S', 'F'};
        auto archive = chm_archive::open(truncated);
        CHECK_FALSE(archive.has_value());
        CHECK(archive.error().code() == error_code::TruncatedArchive);
    }

    TEST_CASE("Parse valid ITSF signature") {
        // Minimal ITSF header structure (enough to pass signature check)
        std::vector<u8> chm_data(128, 0);

        // ITSF signature
        chm_data[0] = 'I';
        chm_data[1] = 'T';
        chm_data[2] = 'S';
        chm_data[3] = 'F';

        // Version (u32)
        chm_data[4] = 3;

        // Header length (u32)
        chm_data[8] = 60;

        auto archive = chm_archive::open(chm_data);
        // May fail at ITSP parsing, but should get past signature
        if (!archive.has_value()) {
            CHECK((archive.error().code() == error_code::TruncatedArchive ||
                   archive.error().code() == error_code::InvalidHeader));
        }
    }

    TEST_CASE("ITSF header parsing") {
        // Create a more complete mock CHM
        std::vector<u8> chm_data(256, 0);

        // ITSF header
        chm_data[0] = 'I'; chm_data[1] = 'T';
        chm_data[2] = 'S'; chm_data[3] = 'F';

        chm_data[4] = 3; // version

        // header_len at offset 8
        *reinterpret_cast<u32*>(&chm_data[8]) = 96;

        // Section 1 offset - point to where ITSP would be
        *reinterpret_cast<u64*>(&chm_data[56]) = 0;    // section0 offset
        *reinterpret_cast<u64*>(&chm_data[64]) = 0;    // section0 length
        *reinterpret_cast<u64*>(&chm_data[72]) = 100;  // section1 offset
        *reinterpret_cast<u64*>(&chm_data[80]) = 100;  // section1 length

        // Put ITSP at offset 100
        if (chm_data.size() > 104) {
            chm_data[100] = 'I'; chm_data[101] = 'T';
            chm_data[102] = 'S'; chm_data[103] = 'P';
        }

        auto archive = chm_archive::open(chm_data);
        // Will likely fail due to incomplete structure but should handle gracefully
    }
}

TEST_SUITE("ChmArchive - Security/CVE Tests") {
    TEST_CASE("CVE-2015-4468 - namelen bounds") {
        auto path = test::chm_dir() / "cve-2015-4468-namelen-bounds.chm";
        REQUIRE(std::filesystem::exists(path));

        auto archive = chm_archive::open(path);
        // Should handle gracefully, no crash or out-of-bounds read
    }

    TEST_CASE("CVE-2015-4469 - namelen bounds") {
        auto path = test::chm_dir() / "cve-2015-4469-namelen-bounds.chm";
        REQUIRE(std::filesystem::exists(path));

        auto archive = chm_archive::open(path);
        // Should handle gracefully
    }

    TEST_CASE("CVE-2015-4472 - namelen bounds") {
        auto path = test::chm_dir() / "cve-2015-4472-namelen-bounds.chm";
        REQUIRE(std::filesystem::exists(path));

        auto archive = chm_archive::open(path);
        // Should handle gracefully
    }

    TEST_CASE("CVE-2017-6419 - LZX negative spaninfo") {
        auto path = test::chm_dir() / "cve-2017-6419-lzx-negative-spaninfo.chm";
        REQUIRE(std::filesystem::exists(path));

        auto archive = chm_archive::open(path);
        // Should handle gracefully
    }

    TEST_CASE("CVE-2018-14679 - off by one") {
        auto path = test::chm_dir() / "cve-2018-14679-off-by-one.chm";
        REQUIRE(std::filesystem::exists(path));

        auto archive = chm_archive::open(path);
        // Should handle gracefully
    }

    TEST_CASE("CVE-2018-14680 - blank filenames") {
        auto path = test::chm_dir() / "cve-2018-14680-blank-filenames.chm";
        REQUIRE(std::filesystem::exists(path));

        auto archive = chm_archive::open(path);
        // Should handle gracefully
    }

    TEST_CASE("CVE-2018-14682 - unicode u100") {
        auto path = test::chm_dir() / "cve-2018-14682-unicode-u100.chm";
        REQUIRE(std::filesystem::exists(path));

        auto archive = chm_archive::open(path);
        // Should handle gracefully
    }

    TEST_CASE("CVE-2018-18585 - blank filenames") {
        auto path = test::chm_dir() / "cve-2018-18585-blank-filenames.chm";
        REQUIRE(std::filesystem::exists(path));

        auto archive = chm_archive::open(path);
        // Should handle gracefully
    }

    TEST_CASE("CVE-2019-1010305 - name overread") {
        auto path = test::chm_dir() / "cve-2019-1010305-name-overread.chm";
        REQUIRE(std::filesystem::exists(path));

        auto archive = chm_archive::open(path);
        // Should handle gracefully, no out-of-bounds read
    }
}

TEST_SUITE("ChmArchive - Encoded Integers") {
    TEST_CASE("Encints 32-bit offsets") {
        auto path = test::chm_dir() / "encints-32bit-offsets.chm";
        REQUIRE(std::filesystem::exists(path));

        auto archive = chm_archive::open(path);
        // Should parse correctly
    }

    TEST_CASE("Encints 32-bit lengths") {
        auto path = test::chm_dir() / "encints-32bit-lengths.chm";
        REQUIRE(std::filesystem::exists(path));

        auto archive = chm_archive::open(path);
        // Should parse correctly
    }

    TEST_CASE("Encints 32-bit both") {
        auto path = test::chm_dir() / "encints-32bit-both.chm";
        REQUIRE(std::filesystem::exists(path));

        auto archive = chm_archive::open(path);
        // Should parse correctly
    }

    TEST_CASE("Encints 64-bit offsets") {
        auto path = test::chm_dir() / "encints-64bit-offsets.chm";
        REQUIRE(std::filesystem::exists(path));

        auto archive = chm_archive::open(path);
        // Should parse correctly
    }

    TEST_CASE("Encints 64-bit lengths") {
        auto path = test::chm_dir() / "encints-64bit-lengths.chm";
        REQUIRE(std::filesystem::exists(path));

        auto archive = chm_archive::open(path);
        // Should parse correctly
    }

    TEST_CASE("Encints 64-bit both") {
        auto path = test::chm_dir() / "encints-64bit-both.chm";
        REQUIRE(std::filesystem::exists(path));

        auto archive = chm_archive::open(path);
        // Should parse correctly
    }
}

TEST_SUITE("ChmArchive - Additional Tests") {
    TEST_CASE("CVE-2015-4467 - reset interval zero (XOR file)") {
        // This is a .chm.xor file, may need special handling
        auto path = test::chm_dir() / "cve-2015-4467-reset-interval-zero.chm.xor";
        REQUIRE(std::filesystem::exists(path));
        // This file has .xor extension - it's an XOR-encoded CHM
        // Just verify we can read the file without crashing
        auto file = file_input_stream::open(path);
        if (file.has_value()) {
            auto size = file->size();
            if (size.has_value()) {
                byte_vector data(*size);
                auto read = file->read(data);
                if (read.has_value()) {
                    // Try to open as CHM - will likely fail due to XOR encoding
                    auto archive = chm_archive::open(data);
                    // Expected to fail with InvalidSignature since it's XOR encoded
                }
            }
        }
    }
}

// Helper to read a file into a byte_vector
static byte_vector read_chm_file(const std::filesystem::path& path) {
    auto file = file_input_stream::open(path);
    if (!file.has_value()) return {};
    auto size = file->size();
    if (!size.has_value()) return {};
    byte_vector data(*size);
    auto read = file->read(data);
    if (!read.has_value()) return {};
    return data;
}

TEST_SUITE("ChmArchive - Functional Tests") {
    TEST_CASE("Open and list main.chm") {
        auto path = test::chm_dir() / "main.chm";
        if (!std::filesystem::exists(path)) {
            MESSAGE("Test file not found, skipping: ", path.string());
            return;
        }

        auto data = read_chm_file(path);
        REQUIRE(!data.empty());

        auto result = chm_archive::open(data);
        REQUIRE(result.has_value());

        auto& archive = *result;
        const auto& files = archive->files();

        // main.chm has 10 files
        CHECK(files.size() == 10);

        // Check for expected files
        bool has_system = false;
        bool has_hhc = false;
        for (const auto& f : files) {
            if (f.name == "#SYSTEM") {
                has_system = true;
                CHECK(f.uncompressed_size == 4216);
                CHECK(f.folder_index == 0);  // section 0 = uncompressed
            }
            if (f.name == "main.hhc") {
                has_hhc = true;
                CHECK(f.uncompressed_size == 684);
                CHECK(f.folder_index == 1);  // section 1 = compressed
            }
        }
        CHECK(has_system);
        CHECK(has_hhc);
        MESSAGE("main.chm parsed successfully with ", files.size(), " files");
    }

    TEST_CASE("Open and list second.chm") {
        auto path = test::chm_dir() / "second.chm";
        if (!std::filesystem::exists(path)) {
            MESSAGE("Test file not found, skipping: ", path.string());
            return;
        }

        auto data = read_chm_file(path);
        REQUIRE(!data.empty());

        auto result = chm_archive::open(data);
        REQUIRE(result.has_value());

        auto& archive = *result;
        const auto& files = archive->files();

        // second.chm has 10 files
        CHECK(files.size() == 10);

        // Check for expected files
        bool has_system = false;
        bool has_hhc = false;
        for (const auto& f : files) {
            if (f.name == "#SYSTEM") {
                has_system = true;
                CHECK(f.uncompressed_size == 4220);
                CHECK(f.folder_index == 0);  // section 0 = uncompressed
            }
            if (f.name == "second.hhc") {
                has_hhc = true;
                CHECK(f.uncompressed_size == 574);
                CHECK(f.folder_index == 1);  // section 1 = compressed
            }
        }
        CHECK(has_system);
        CHECK(has_hhc);
        MESSAGE("second.chm parsed successfully with ", files.size(), " files");
    }

    TEST_CASE("Open and list putty.chm") {
        auto path = test::chm_dir() / "putty.chm";
        if (!std::filesystem::exists(path)) {
            MESSAGE("Test file not found, skipping: ", path.string());
            return;
        }

        auto data = read_chm_file(path);
        REQUIRE(!data.empty());

        auto result = chm_archive::open(data);
        REQUIRE(result.has_value());

        auto& archive = *result;
        const auto& files = archive->files();

        // putty.chm has 457 files
        CHECK(files.size() == 457);

        // Check for expected files
        bool has_system = false;
        bool has_index = false;
        bool has_css = false;
        size_t html_count = 0;

        for (const auto& f : files) {
            if (f.name == "#SYSTEM") {
                has_system = true;
                CHECK(f.uncompressed_size == 4266);
                CHECK(f.folder_index == 0);  // section 0 = uncompressed
            }
            if (f.name == "index.html") {
                has_index = true;
                CHECK(f.folder_index == 1);  // section 1 = compressed
            }
            if (f.name == "chm.css") {
                has_css = true;
                CHECK(f.uncompressed_size == 254);
            }
            if (f.name.size() > 5 && f.name.substr(f.name.size() - 5) == ".html") {
                html_count++;
            }
        }
        CHECK(has_system);
        CHECK(has_index);
        CHECK(has_css);
        CHECK(html_count > 400);  // Many HTML help pages
        MESSAGE("putty.chm parsed successfully with ", files.size(), " files (", html_count, " HTML)");
    }

    TEST_CASE("Extract uncompressed section 0 file (#SYSTEM)") {
        auto path = test::chm_dir() / "main.chm";
        if (!std::filesystem::exists(path)) {
            MESSAGE("Test file not found, skipping: ", path.string());
            return;
        }

        auto data = read_chm_file(path);
        REQUIRE(!data.empty());

        auto result = chm_archive::open(data);
        REQUIRE(result.has_value());

        auto& archive = *result;
        const auto& files = archive->files();

        // Find and extract #SYSTEM (section 0, uncompressed)
        for (const auto& f : files) {
            if (f.name == "#SYSTEM" && f.folder_index == 0) {
                auto extract_result = archive->extract(f);
                REQUIRE(extract_result.has_value());
                CHECK(extract_result->size() == f.uncompressed_size);
                MESSAGE("Extracted #SYSTEM: ", extract_result->size(), " bytes");
                break;
            }
        }
    }

    TEST_CASE("Extract compressed section 1 file returns error") {
        auto path = test::chm_dir() / "main.chm";
        if (!std::filesystem::exists(path)) {
            MESSAGE("Test file not found, skipping: ", path.string());
            return;
        }

        auto data = read_chm_file(path);
        REQUIRE(!data.empty());

        auto result = chm_archive::open(data);
        REQUIRE(result.has_value());

        auto& archive = *result;
        const auto& files = archive->files();

        // Find and try to extract main.hhc (section 1, compressed)
        for (const auto& f : files) {
            if (f.name == "main.hhc" && f.folder_index == 1) {
                auto extract_result = archive->extract(f);
                // Should fail because LZX decompression is not implemented
                CHECK_FALSE(extract_result.has_value());
                CHECK(extract_result.error().code() == error_code::UnsupportedCompression);
                MESSAGE("Section 1 extraction correctly returns UnsupportedCompression error");
                break;
            }
        }
    }
}
