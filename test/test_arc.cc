#include <doctest/doctest.h>
#include <crate/formats/arc.hh>
#include <crate/formats/arc_internal.hh>
#include <crate/core/crc.hh>
#include <crate/test_config.hh>
#include <array>
#include <fstream>
#include <cstring>

using namespace crate;

namespace {
    const auto CORPUS_DIR = test::arc_dir();
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

TEST_SUITE("ArcArchive - Basic") {
    TEST_CASE("Invalid data") {
        std::array<u8, 16> invalid_data = {0};
        auto archive = arc_archive::open(invalid_data);
        CHECK(archive.has_value());
        CHECK((*archive)->files().empty());
    }

    TEST_CASE("Empty data") {
        byte_span empty;
        auto archive = arc_archive::open(empty);
        CHECK(archive.has_value());
        CHECK((*archive)->files().empty());
    }
}

TEST_SUITE("ArcArchive - CRC16") {
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

TEST_SUITE("ArcArchive - RLE90") {
    TEST_CASE("No escape sequences") {
        std::array<u8, 5> input = {'H', 'e', 'l', 'l', 'o'};
        auto output = arc::unpack_rle(input);
        CHECK(output.size() == 5);
        CHECK(std::memcmp(output.data(), "Hello", 5) == 0);
    }

    TEST_CASE("Literal DLE") {
        std::array<u8, 4> input = {'A', 0x90, 0x00, 'B'};
        auto output = arc::unpack_rle(input);
        CHECK(output.size() == 3);
        CHECK(output[0] == 'A');
        CHECK(output[1] == 0x90);
        CHECK(output[2] == 'B');
    }

    TEST_CASE("Run of characters") {
        std::array<u8, 3> input = {'A', 0x90, 0x05};
        auto output = arc::unpack_rle(input);
        CHECK(output.size() == 5);
        for (size_t i = 0; i < 5; i++) {
            CHECK(output[i] == 'A');
        }
    }
}

TEST_SUITE("ArcArchive - Corpus") {
    TEST_CASE("Method 2 - Stored (Unpacked)") {
        auto archive_path = CORPUS_DIR / "store.arc";
        REQUIRE(std::filesystem::exists(archive_path));

        auto expected = read_file(LICENSE_FILE);
        REQUIRE(expected.size() == 11357);

        auto archive = arc_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->files().size() >= 1);

        const auto& entry = (*archive)->files()[0];
        CHECK(entry.name == "LICENSE");
        CHECK(entry.uncompressed_size == 11357);

        auto actual = (*archive)->extract(entry);
        REQUIRE(actual.has_value());
        CHECK(compare_data(expected, *actual));
    }

    TEST_CASE("Method 3 - Packed (RLE90)") {
        auto archive_path = CORPUS_DIR / "cpm.arc";
        REQUIRE(std::filesystem::exists(archive_path));

        auto expected = read_file(CORPUS_DIR / "READ.COM");
        REQUIRE(expected.size() == 128);

        auto archive = arc_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->files().size() >= 2);

        const file_entry* read_entry = nullptr;
        for (const auto& entry : (*archive)->files()) {
            if (entry.name == "READ.COM") {
                read_entry = &entry;
                break;
            }
        }
        REQUIRE(read_entry != nullptr);

        auto actual = (*archive)->extract(*read_entry);
        REQUIRE(actual.has_value());
        CHECK(compare_data(expected, *actual));
    }

    TEST_CASE("Method 4 - Squeezed (Huffman)") {
        auto archive_path = CORPUS_DIR / "cpm.arc";
        REQUIRE(std::filesystem::exists(archive_path));

        auto expected = read_file(CORPUS_DIR / "DDTZ.COM");
        REQUIRE(expected.size() == 9984);

        auto archive = arc_archive::open(archive_path);
        REQUIRE(archive.has_value());

        const file_entry* ddtz_entry = nullptr;
        for (const auto& entry : (*archive)->files()) {
            if (entry.name == "DDTZ.COM") {
                ddtz_entry = &entry;
                break;
            }
        }
        REQUIRE(ddtz_entry != nullptr);

        auto actual = (*archive)->extract(*ddtz_entry);
        REQUIRE(actual.has_value());
        CHECK(compare_data(expected, *actual));
    }

    TEST_CASE("Method 8 - Crunched (LZW + RLE)") {
        auto archive_path = CORPUS_DIR / "crunch.arc";
        REQUIRE(std::filesystem::exists(archive_path));

        auto expected = read_file(LICENSE_FILE);
        REQUIRE(expected.size() == 11357);

        auto archive = arc_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->files().size() >= 1);

        const auto& entry = (*archive)->files()[0];
        auto actual = (*archive)->extract(entry);
        REQUIRE(actual.has_value());
        CHECK(compare_data(expected, *actual));
    }

    TEST_CASE("Method 9 - Squashed (LZW 13-bit)") {
        auto archive_path = CORPUS_DIR / "squashed.arc";
        REQUIRE(std::filesystem::exists(archive_path));

        auto expected = read_file(LICENSE_FILE);
        REQUIRE(expected.size() == 11357);

        auto archive = arc_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->files().size() >= 1);

        const auto& entry = (*archive)->files()[0];
        auto actual = (*archive)->extract(entry);
        REQUIRE(actual.has_value());
        CHECK(compare_data(expected, *actual));
    }

    TEST_CASE("Wrong CRC16 - should fail extraction") {
        auto archive_path = CORPUS_DIR / "wrongcrc16.arc";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = arc_archive::open(archive_path);
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
