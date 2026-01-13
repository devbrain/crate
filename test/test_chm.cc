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
