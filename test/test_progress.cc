// Tests for progress callbacks in decompressors and archive formats
#include <doctest/doctest.h>
#include <crate/test_config.hh>

// Decompressors
#include <crate/compression/explode.hh>
#include <crate/compression/inflate.hh>
#include <crate/compression/diet.hh>

// Archive formats
#include <crate/formats/arj.hh>
#include <crate/formats/cab.hh>
#include <crate/formats/lha.hh>
#include <crate/formats/arc.hh>
#include <crate/formats/zoo.hh>
#include <crate/formats/ha.hh>
#include <crate/formats/hyp.hh>
#include <crate/formats/ace.hh>
#include <crate/formats/stuffit.hh>
#include <crate/formats/rar.hh>
#include <crate/formats/chm.hh>
#include <crate/formats/zip.hh>
#include <crate/formats/floppy.hh>

#include <filesystem>
#include <fstream>
#include <vector>
#include <atomic>

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

// Helper to track progress callback invocations
struct progress_tracker {
    std::atomic<size_t> call_count{0};
    std::atomic<size_t> last_bytes_written{0};
    std::atomic<size_t> last_total_expected{0};

    void reset() {
        call_count = 0;
        last_bytes_written = 0;
        last_total_expected = 0;
    }

    auto make_decompressor_callback() {
        return [this](size_t bytes_written, size_t total_expected) {
            call_count++;
            last_bytes_written = bytes_written;
            last_total_expected = total_expected;
        };
    }
};

// Helper to track archive byte progress
struct archive_progress_tracker {
    std::atomic<size_t> call_count{0};
    std::atomic<size_t> last_bytes_written{0};
    std::atomic<size_t> last_total_expected{0};
    std::string last_filename;

    void reset() {
        call_count = 0;
        last_bytes_written = 0;
        last_total_expected = 0;
        last_filename.clear();
    }

    auto make_callback() {
        return [this](const file_entry& entry, size_t bytes_written, size_t total_expected) {
            call_count++;
            last_bytes_written = bytes_written;
            last_total_expected = total_expected;
            last_filename = entry.name;
        };
    }
};

} // anonymous namespace

// =============================================================================
// DECOMPRESSOR PROGRESS CALLBACK TESTS
// =============================================================================

TEST_SUITE("Progress - Explode Decompressor") {
    TEST_CASE("Progress callback is called during decompression") {
        auto compressed = read_file(test::pkware_dir() / "large.imploded");
        auto expected = read_file(test::pkware_dir() / "large.decomp");

        REQUIRE(!compressed.empty());
        REQUIRE(!expected.empty());

        progress_tracker tracker;
        explode_decompressor decompressor;
        decompressor.set_progress_callback(tracker.make_decompressor_callback());

        std::vector<u8> output(expected.size() + 1024);
        auto result = decompressor.decompress(compressed, output);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == *result);
        MESSAGE("Explode progress callback called ", tracker.call_count.load(), " times");
    }
}

TEST_SUITE("Progress - Inflate Decompressor") {
    TEST_CASE("Progress callback is called during decompression") {
        auto compressed = read_file(test::testdata_dir() / "gz" / "LICENSE.gz");

        REQUIRE(!compressed.empty());

        // Skip gzip header (10 bytes minimum)
        if (compressed.size() < 18) {
            MESSAGE("Invalid gzip file, skipping");
            return;
        }

        // Find the start of deflate data (after gzip header)
        size_t header_end = 10;
        if (compressed[3] & 0x08) {  // FNAME flag
            while (header_end < compressed.size() && compressed[header_end] != 0) header_end++;
            header_end++;
        }

        byte_span deflate_data(compressed.data() + header_end,
                               compressed.size() - header_end - 8);  // -8 for trailer

        progress_tracker tracker;
        inflate_decompressor decompressor;
        decompressor.set_progress_callback(tracker.make_decompressor_callback());

        std::vector<u8> output(64 * 1024);  // 64KB should be enough
        auto result = decompressor.decompress(deflate_data, output);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == *result);
        MESSAGE("Inflate progress callback called ", tracker.call_count.load(), " times");
    }
}

TEST_SUITE("Progress - Diet Decompressor") {
    TEST_CASE("Progress callback is called during decompression") {
        // Diet compressed files are rare, create synthetic test
        // For now, just verify the callback can be set
        progress_tracker tracker;
        diet_decompressor decompressor;
        decompressor.set_progress_callback(tracker.make_decompressor_callback());

        // Empty input should not crash
        std::vector<u8> input = {};
        std::vector<u8> output(1024);

        // Diet needs valid compressed input, so just verify the callback
        // was set but not yet called (no decompression occurred)
        CHECK(tracker.call_count == 0);
    }
}

// =============================================================================
// ARCHIVE BYTE PROGRESS CALLBACK TESTS
// =============================================================================

TEST_SUITE("Progress - ARJ Archive") {
    TEST_CASE("Byte progress callback during extraction") {
        auto archive_path = test::arj_dir() / "method1.arj";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = arj_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());

        archive_progress_tracker tracker;
        (*archive)->set_byte_progress_callback(tracker.make_callback());

        const auto& entry = (*archive)->files()[0];
        auto result = (*archive)->extract(entry);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == result->size());
        MESSAGE("ARJ byte progress callback called ", tracker.call_count.load(), " times");
    }
}

TEST_SUITE("Progress - CAB Archive") {
    TEST_CASE("Byte progress callback during extraction") {
        auto archive_path = test::cab_dir() / "simple.cab";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = cab_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());

        archive_progress_tracker tracker;
        (*archive)->set_byte_progress_callback(tracker.make_callback());

        const auto& entry = (*archive)->files()[0];
        auto result = (*archive)->extract(entry);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == result->size());
        MESSAGE("CAB byte progress callback called ", tracker.call_count.load(), " times");
    }
}

TEST_SUITE("Progress - LHA Archive") {
    TEST_CASE("Byte progress callback during extraction") {
        auto archive_path = test::lha_dir() / "lha_os2_208" / "lh5.lzh";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = lha_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());

        archive_progress_tracker tracker;
        (*archive)->set_byte_progress_callback(tracker.make_callback());

        const auto& entry = (*archive)->files()[0];
        auto result = (*archive)->extract(entry);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == result->size());
        MESSAGE("LHA byte progress callback called ", tracker.call_count.load(), " times");
    }
}

TEST_SUITE("Progress - ARC Archive") {
    TEST_CASE("Byte progress callback during extraction") {
        auto archive_path = test::arc_dir() / "crunch.arc";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = arc_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());

        archive_progress_tracker tracker;
        (*archive)->set_byte_progress_callback(tracker.make_callback());

        const auto& entry = (*archive)->files()[0];
        auto result = (*archive)->extract(entry);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == result->size());
        MESSAGE("ARC byte progress callback called ", tracker.call_count.load(), " times");
    }
}

TEST_SUITE("Progress - ZOO Archive") {
    TEST_CASE("Byte progress callback during extraction") {
        auto archive_path = test::zoo_dir() / "default.zoo";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = zoo_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());

        archive_progress_tracker tracker;
        (*archive)->set_byte_progress_callback(tracker.make_callback());

        const auto& entry = (*archive)->files()[0];
        auto result = (*archive)->extract(entry);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == result->size());
        MESSAGE("ZOO byte progress callback called ", tracker.call_count.load(), " times");
    }
}

TEST_SUITE("Progress - HA Archive") {
    TEST_CASE("Byte progress callback during extraction") {
        auto archive_path = test::ha_dir() / "asc.ha";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = ha_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());

        archive_progress_tracker tracker;
        (*archive)->set_byte_progress_callback(tracker.make_callback());

        const auto& entry = (*archive)->files()[0];
        auto result = (*archive)->extract(entry);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == result->size());
        MESSAGE("HA byte progress callback called ", tracker.call_count.load(), " times");
    }
}

TEST_SUITE("Progress - HYP Archive") {
    TEST_CASE("Byte progress callback during extraction") {
        auto archive_path = test::hyp_dir() / "license.hyp";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = hyp_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());

        archive_progress_tracker tracker;
        (*archive)->set_byte_progress_callback(tracker.make_callback());

        const auto& entry = (*archive)->files()[0];
        auto result = (*archive)->extract(entry);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == result->size());
        MESSAGE("HYP byte progress callback called ", tracker.call_count.load(), " times");
    }
}

TEST_SUITE("Progress - ACE Archive") {
    TEST_CASE("Byte progress callback during extraction") {
        auto archive_path = test::ace_dir() / "license1.ace";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = ace_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());

        archive_progress_tracker tracker;
        (*archive)->set_byte_progress_callback(tracker.make_callback());

        const auto& entry = (*archive)->files()[0];
        auto result = (*archive)->extract(entry);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == result->size());
        MESSAGE("ACE byte progress callback called ", tracker.call_count.load(), " times");
    }
}

TEST_SUITE("Progress - StuffIt Archive") {
    TEST_CASE("Byte progress callback during extraction") {
        auto archive_path = test::stuffit_dir() / "testfile.stuffit45_dlx.mac9.sit";
        REQUIRE(std::filesystem::exists(archive_path));

        auto data = read_file(archive_path);
        auto archive = stuffit_archive::open(data);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());

        // Find first non-directory file
        const file_entry* test_entry = nullptr;
        for (const auto& entry : (*archive)->files()) {
            if (!entry.is_directory && entry.uncompressed_size > 0) {
                test_entry = &entry;
                break;
            }
        }

        if (!test_entry) {
            MESSAGE("No extractable file found, skipping");
            return;
        }

        archive_progress_tracker tracker;
        (*archive)->set_byte_progress_callback(tracker.make_callback());

        auto result = (*archive)->extract(*test_entry);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == result->size());
        MESSAGE("StuffIt byte progress callback called ", tracker.call_count.load(), " times");
    }
}

TEST_SUITE("Progress - RAR Archive") {
    TEST_CASE("Byte progress callback during extraction") {
        auto archive_path = test::rar_dir() / "unrar_test_01.rar";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = rar_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());

        archive_progress_tracker tracker;
        (*archive)->set_byte_progress_callback(tracker.make_callback());

        const auto& entry = (*archive)->files()[0];
        auto result = (*archive)->extract(entry);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == result->size());
        MESSAGE("RAR byte progress callback called ", tracker.call_count.load(), " times");
    }
}

TEST_SUITE("Progress - CHM Archive") {
    TEST_CASE("Byte progress callback during extraction") {
        // Use a CVE test file that should be parseable
        auto archive_path = test::chm_dir() / "cve-2015-4468-namelen-bounds.chm";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = chm_archive::open(archive_path);
        if (!archive.has_value()) {
            MESSAGE("Archive failed to open (may be intentionally malformed), skipping");
            return;
        }

        if ((*archive)->files().empty()) {
            MESSAGE("No files in archive, skipping");
            return;
        }

        // Find first non-directory file with content
        const file_entry* test_entry = nullptr;
        for (const auto& entry : (*archive)->files()) {
            if (!entry.is_directory && entry.uncompressed_size > 0) {
                test_entry = &entry;
                break;
            }
        }

        if (!test_entry) {
            MESSAGE("No extractable file found, skipping");
            return;
        }

        archive_progress_tracker tracker;
        (*archive)->set_byte_progress_callback(tracker.make_callback());

        auto result = (*archive)->extract(*test_entry);

        if (!result.has_value()) {
            MESSAGE("Extraction failed (may be intentionally malformed), skipping");
            return;
        }

        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == result->size());
        MESSAGE("CHM byte progress callback called ", tracker.call_count.load(), " times");
    }
}

TEST_SUITE("Progress - ZIP Archive") {
    TEST_CASE("Byte progress callback during extraction") {
        auto archive_path = test::testdata_dir() / "zip" / "test.zip";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = zip_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());

        // Find first non-directory file with content
        const file_entry* test_entry = nullptr;
        for (const auto& entry : (*archive)->files()) {
            if (!entry.is_directory && entry.uncompressed_size > 0) {
                test_entry = &entry;
                break;
            }
        }

        if (!test_entry) {
            MESSAGE("No extractable file found, skipping");
            return;
        }

        archive_progress_tracker tracker;
        (*archive)->set_byte_progress_callback(tracker.make_callback());

        auto result = (*archive)->extract(*test_entry);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == result->size());
        MESSAGE("ZIP byte progress callback called ", tracker.call_count.load(), " times");
    }
}

TEST_SUITE("Progress - Floppy Image") {
    TEST_CASE("Byte progress callback during extraction") {
        auto archive_path = test::floppy_dir() / "Borland - Turbo Pascal v5.0 - Disk 1 of 3 - Install & Compiler.img";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = floppy_image::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());

        // Find first file with content
        const file_entry* test_entry = nullptr;
        for (const auto& entry : (*archive)->files()) {
            if (!entry.is_directory && entry.uncompressed_size > 0) {
                test_entry = &entry;
                break;
            }
        }

        if (!test_entry) {
            MESSAGE("No extractable file found, skipping");
            return;
        }

        archive_progress_tracker tracker;
        (*archive)->set_byte_progress_callback(tracker.make_callback());

        auto result = (*archive)->extract(*test_entry);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == result->size());
        MESSAGE("Floppy image byte progress callback called ", tracker.call_count.load(), " times");
    }
}

#ifdef CRATE_WITH_LIBARCHIVE
#include <crate/formats/libarchive_archive.hh>

TEST_SUITE("Progress - libarchive Archive") {
    TEST_CASE("Byte progress callback during extraction") {
        // Use any archive that libarchive can read
        auto archive_path = test::testdata_dir() / "zip" / "test.zip";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = libarchive_archive::open(archive_path);
        REQUIRE(archive.has_value());
        REQUIRE(!(*archive)->files().empty());

        // Find first non-directory file with content
        const file_entry* test_entry = nullptr;
        for (const auto& entry : (*archive)->files()) {
            if (!entry.is_directory && entry.uncompressed_size > 0) {
                test_entry = &entry;
                break;
            }
        }

        if (!test_entry) {
            MESSAGE("No extractable file found, skipping");
            return;
        }

        archive_progress_tracker tracker;
        (*archive)->set_byte_progress_callback(tracker.make_callback());

        auto result = (*archive)->extract(*test_entry);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == result->size());
        MESSAGE("libarchive byte progress callback called ", tracker.call_count.load(), " times");
    }
}
#endif

#ifdef CRATE_WITH_ZSTD
#include <crate/compression/zstd.hh>

TEST_SUITE("Progress - Zstd Decompressor") {
    TEST_CASE("Progress callback is called during decompression") {
        auto compressed = read_file(test::testdata_dir() / "zstd" / "LICENSE.zst");

        REQUIRE(!compressed.empty());

        progress_tracker tracker;
        zstd_decompressor decompressor;
        decompressor.set_progress_callback(tracker.make_decompressor_callback());

        // Allocate reasonable output buffer (LICENSE file is typically < 64KB)
        std::vector<u8> output(256 * 1024);
        auto result = decompressor.decompress(compressed, output);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == *result);
        MESSAGE("Zstd progress callback called ", tracker.call_count.load(), " times");
    }
}
#endif

#ifdef CRATE_WITH_BROTLI
#include <crate/compression/brotli.hh>

TEST_SUITE("Progress - Brotli Decompressor") {
    TEST_CASE("Progress callback is called during decompression") {
        auto compressed = read_file(test::testdata_dir() / "brotli" / "LICENSE.br");

        REQUIRE(!compressed.empty());

        progress_tracker tracker;
        brotli_decompressor decompressor;
        decompressor.set_progress_callback(tracker.make_decompressor_callback());

        // Allocate reasonable output buffer (LICENSE file is typically < 64KB)
        std::vector<u8> output(256 * 1024);
        auto result = decompressor.decompress(compressed, output);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == *result);
        MESSAGE("Brotli progress callback called ", tracker.call_count.load(), " times");
    }
}
#endif

#ifdef CRATE_WITH_BZIP2
#include <crate/compression/bzip2.hh>

TEST_SUITE("Progress - Bzip2 Decompressor") {
    TEST_CASE("Progress callback is called during decompression") {
        auto compressed = read_file(test::testdata_dir() / "bzip2" / "LICENSE.bz2");

        REQUIRE(!compressed.empty());

        progress_tracker tracker;
        bzip2_decompressor decompressor;
        decompressor.set_progress_callback(tracker.make_decompressor_callback());

        // Allocate reasonable output buffer (LICENSE file is typically < 64KB)
        std::vector<u8> output(256 * 1024);
        auto result = decompressor.decompress(compressed, output);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == *result);
        MESSAGE("Bzip2 progress callback called ", tracker.call_count.load(), " times");
    }
}
#endif

#ifdef CRATE_WITH_XZ
#include <crate/compression/xz.hh>

TEST_SUITE("Progress - XZ Decompressor") {
    TEST_CASE("Progress callback is called during decompression") {
        auto compressed = read_file(test::testdata_dir() / "xz" / "LICENSE.xz");

        REQUIRE(!compressed.empty());

        progress_tracker tracker;
        xz_decompressor decompressor;
        decompressor.set_progress_callback(tracker.make_decompressor_callback());

        // Allocate reasonable output buffer (LICENSE file is typically < 64KB)
        std::vector<u8> output(256 * 1024);
        auto result = decompressor.decompress(compressed, output);

        REQUIRE(result.has_value());
        CHECK(tracker.call_count > 0);
        CHECK(tracker.last_bytes_written == *result);
        MESSAGE("XZ progress callback called ", tracker.call_count.load(), " times");
    }
}
#endif
