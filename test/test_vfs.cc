#include <doctest/doctest.h>
#include <crate/crate.hh>
#include <crate/formats/ace.hh>
#include <crate/formats/arc.hh>
#include <crate/formats/arj.hh>
#include <crate/formats/cab.hh>
#include <crate/formats/ha.hh>
#include <crate/formats/hyp.hh>
#include <crate/formats/lha.hh>
#include <crate/formats/rar.hh>
#include <crate/formats/zoo.hh>
#include <crate/test_config.hh>
#include <fstream>
#include <cstring>

using namespace crate;
using namespace crate::vfs;

namespace {
    byte_vector read_file(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return byte_vector((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
    }

    // Compare streaming read with direct read
    bool verify_streaming_read(file_reader& reader, const byte_vector& expected) {
        byte_vector streamed;
        u8 buffer[1024];

        while (!reader.eof()) {
            auto result = reader.read(buffer, sizeof(buffer));
            if (!result) {
                return false;
            }
            if (*result == 0) break;
            streamed.insert(streamed.end(), buffer, buffer + *result);
        }

        if (streamed.size() != expected.size()) {
            return false;
        }
        return std::memcmp(streamed.data(), expected.data(), expected.size()) == 0;
    }
}

TEST_SUITE("VFS - Core") {
    TEST_CASE("Path normalization") {
        // Test internal path handling through exists() checks
        // Create a simple archive to test with
        auto lha_path = test::lha_dir() / "lha_unix114i" / "h0_lh0.lzh";
        REQUIRE(std::filesystem::exists(lha_path));

        auto archive = lha_archive::open(lha_path);
        REQUIRE(archive.has_value());

        auto vfs = archive_vfs::create(std::move(*archive));
        REQUIRE(vfs.has_value());

        // Root should always exist
        CHECK((*vfs)->exists(""));
        CHECK((*vfs)->is_directory(""));

        // Check file count matches
        auto root_entries = (*vfs)->root();
        REQUIRE(root_entries.has_value());
        CHECK(!root_entries->empty());
    }
}

TEST_SUITE("VFS - LHA") {
    TEST_CASE("LHA VFS browsing and streaming") {
        auto archive_dir = test::lha_dir() / "lha_unix114i";
        REQUIRE(std::filesystem::exists(archive_dir));

        for (const auto& entry : std::filesystem::directory_iterator(archive_dir)) {
            auto ext = entry.path().extension().string();
            if (ext != ".lzh" && ext != ".lha") continue;

            auto archive = lha_archive::open(entry.path());
            if (!archive.has_value()) continue;

            // Store files for direct extraction comparison
            std::vector<std::pair<std::string, byte_vector>> extracted_files;
            for (const auto& file : (*archive)->files()) {
                if (file.uncompressed_size == 0) continue;  // Skip directories
                auto data = (*archive)->extract(file);
                if (data.has_value()) {
                    extracted_files.emplace_back(file.name, std::move(*data));
                }
            }

            // Reopen for VFS test
            archive = lha_archive::open(entry.path());
            REQUIRE(archive.has_value());

            auto vfs = archive_vfs::create(std::move(*archive));
            REQUIRE(vfs.has_value());

            // Test VFS operations
            CHECK((*vfs)->exists(""));
            CHECK((*vfs)->is_directory(""));

            // Verify each file through streaming read
            for (const auto& [path, expected] : extracted_files) {
                CAPTURE(path);
                CHECK((*vfs)->exists(path));
                CHECK((*vfs)->is_file(path));

                // Test stat
                auto st = (*vfs)->stat(path);
                REQUIRE(st.has_value());
                CHECK(st->type == entry_type::File);
                CHECK(st->size == expected.size());

                // Test streaming read produces same result
                auto reader = (*vfs)->open(path);
                REQUIRE(reader.has_value());
                CHECK(verify_streaming_read(**reader, expected));

                // Test direct read
                auto direct = (*vfs)->read(path);
                REQUIRE(direct.has_value());
                CHECK(direct->size() == expected.size());
                CHECK(std::memcmp(direct->data(), expected.data(), expected.size()) == 0);
            }

        }
    }
}

TEST_SUITE("VFS - RAR") {
    TEST_CASE("RAR5 VFS browsing and streaming") {
        auto archive_path = test::rar_dir() / "unrar_test_01.rar";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = rar_archive::open(archive_path);
        REQUIRE(archive.has_value());

        // Store files for comparison
        std::vector<std::pair<std::string, byte_vector>> extracted_files;
        for (const auto& file : (*archive)->files()) {
            auto data = (*archive)->extract(file);
            if (data.has_value()) {
                extracted_files.emplace_back(file.name, std::move(*data));
            }
        }

        // Reopen for VFS
        archive = rar_archive::open(archive_path);
        REQUIRE(archive.has_value());

        auto vfs = archive_vfs::create(std::move(*archive));
        REQUIRE(vfs.has_value());

        // Verify streaming produces same results
        for (const auto& [path, expected] : extracted_files) {
            CAPTURE(path);

            auto reader = (*vfs)->open(path);
            REQUIRE(reader.has_value());
            CHECK((*reader)->size() == expected.size());
            CHECK(verify_streaming_read(**reader, expected));
        }
    }
}

TEST_SUITE("VFS - CAB") {
    TEST_CASE("CAB VFS browsing and streaming") {
        auto cab_dir = test::cab_dir();
        REQUIRE(std::filesystem::exists(cab_dir));

        int tested = 0;
        for (const auto& entry : std::filesystem::directory_iterator(cab_dir)) {
            if (entry.path().extension() != ".cab") continue;

            auto archive = cab_archive::open(entry.path());
            if (!archive.has_value()) continue;

            // Store files for comparison
            std::vector<std::pair<std::string, byte_vector>> extracted_files;
            for (const auto& file : (*archive)->files()) {
                auto data = (*archive)->extract(file);
                if (data.has_value()) {
                    extracted_files.emplace_back(file.name, std::move(*data));
                }
            }

            if (extracted_files.empty()) continue;

            // Reopen for VFS
            archive = cab_archive::open(entry.path());
            REQUIRE(archive.has_value());

            auto vfs = archive_vfs::create(std::move(*archive));
            REQUIRE(vfs.has_value());

            // Verify streaming
            for (const auto& [path, expected] : extracted_files) {
                auto reader = (*vfs)->open(path);
                if (!reader.has_value()) continue;

                CHECK(verify_streaming_read(**reader, expected));
            }

            tested++;
            if (tested >= 3) break;  // Test a few CAB files
        }
        CHECK(tested > 0);
    }
}

TEST_SUITE("VFS - Directory Structure") {
    TEST_CASE("Directory tree building") {
        // Create an archive with nested paths to test directory structure
        auto lha_path = test::lha_dir() / "lha_unix114i" / "h0_lh5.lzh";
        REQUIRE(std::filesystem::exists(lha_path));

        auto archive = lha_archive::open(lha_path);
        REQUIRE(archive.has_value());

        auto vfs = archive_vfs::create(std::move(*archive));
        REQUIRE(vfs.has_value());

        // Test root listing
        auto root = (*vfs)->root();
        REQUIRE(root.has_value());
        CHECK(!root->empty());
    }

    TEST_CASE("Non-existent path handling") {
        auto lha_path = test::lha_dir() / "lha_unix114i" / "h0_lh0.lzh";
        REQUIRE(std::filesystem::exists(lha_path));

        auto archive = lha_archive::open(lha_path);
        REQUIRE(archive.has_value());

        auto vfs = archive_vfs::create(std::move(*archive));
        REQUIRE(vfs.has_value());

        // Non-existent paths
        CHECK_FALSE((*vfs)->exists("nonexistent"));
        CHECK_FALSE((*vfs)->exists("foo/bar/baz"));
        CHECK_FALSE((*vfs)->is_file("nonexistent"));
        CHECK_FALSE((*vfs)->is_directory("nonexistent"));

        // stat returns nullopt for non-existent
        CHECK_FALSE((*vfs)->stat("nonexistent").has_value());

        // read returns error for non-existent
        auto result = (*vfs)->read("nonexistent");
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code() == error_code::FileNotInArchive);

        // open returns error for non-existent
        auto reader = (*vfs)->open("nonexistent");
        CHECK_FALSE(reader.has_value());
    }
}

TEST_SUITE("VFS - FileReader") {
    TEST_CASE("FileReader position tracking") {
        auto lha_path = test::lha_dir() / "lha_unix114i" / "h0_lh5.lzh";
        REQUIRE(std::filesystem::exists(lha_path));

        auto archive = lha_archive::open(lha_path);
        REQUIRE(archive.has_value());

        // Get first non-empty file
        std::string test_path;
        u64 test_size = 0;
        for (const auto& file : (*archive)->files()) {
            if (file.uncompressed_size > 100) {
                test_path = file.name;
                test_size = file.uncompressed_size;
                break;
            }
        }

        if (test_path.empty()) {
            return;
        }

        // Reopen for VFS
        archive = lha_archive::open(lha_path);
        auto vfs = archive_vfs::create(std::move(*archive));
        REQUIRE(vfs.has_value());

        auto reader = (*vfs)->open(test_path);
        REQUIRE(reader.has_value());

        // Initial state
        CHECK((*reader)->position() == 0);
        CHECK((*reader)->size() == test_size);
        CHECK((*reader)->remaining() == test_size);
        CHECK_FALSE((*reader)->eof());

        // Read some bytes
        u8 buffer[50];
        auto n = (*reader)->read(buffer, sizeof(buffer));
        REQUIRE(n.has_value());

        CHECK((*reader)->position() == *n);
        CHECK((*reader)->remaining() == test_size - *n);

        // Read to end
        while (!(*reader)->eof()) {
            n = (*reader)->read(buffer, sizeof(buffer));
            if (!n.has_value() || *n == 0) break;
        }

        CHECK((*reader)->eof());
        CHECK((*reader)->position() == test_size);
        CHECK((*reader)->remaining() == 0);

        // Reading at EOF returns 0
        n = (*reader)->read(buffer, sizeof(buffer));
        REQUIRE(n.has_value());
        CHECK(*n == 0);
    }

    TEST_CASE("FileReader vector read") {
        auto lha_path = test::lha_dir() / "lha_unix114i" / "h0_lh0.lzh";
        REQUIRE(std::filesystem::exists(lha_path));

        auto archive = lha_archive::open(lha_path);
        REQUIRE(archive.has_value());

        // Find a test file
        std::string test_path;
        for (const auto& file : (*archive)->files()) {
            if (file.uncompressed_size > 0) {
                test_path = file.name;
                break;
            }
        }

        if (test_path.empty()) return;

        // Get expected data
        byte_vector expected;
        for (const auto& file : (*archive)->files()) {
            if (file.name == test_path) {
                auto data = (*archive)->extract(file);
                if (data.has_value()) expected = std::move(*data);
                break;
            }
        }

        // Reopen for VFS
        archive = lha_archive::open(lha_path);
        auto vfs = archive_vfs::create(std::move(*archive));
        REQUIRE(vfs.has_value());

        auto reader = (*vfs)->open(test_path);
        REQUIRE(reader.has_value());

        // Read using vector interface
        std::vector<u8> buffer;
        while (!(*reader)->eof()) {
            auto n = (*reader)->read(buffer, 256);
            if (!n.has_value() || *n == 0) break;
        }

        CHECK(buffer.size() == expected.size());
        if (!buffer.empty()) {
            CHECK(std::memcmp(buffer.data(), expected.data(), expected.size()) == 0);
        }
    }
}

TEST_SUITE("VFS - Multi-format") {
    TEST_CASE("ARJ VFS") {
        auto archive_path = test::arj_dir() / "method1.arj";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = arj_archive::open(archive_path);
        REQUIRE(archive.has_value());

        std::vector<std::pair<std::string, byte_vector>> extracted;
        for (const auto& file : (*archive)->files()) {
            auto data = (*archive)->extract(file);
            if (data.has_value()) {
                extracted.emplace_back(file.name, std::move(*data));
            }
        }

        archive = arj_archive::open(archive_path);
        auto vfs = archive_vfs::create(std::move(*archive));
        REQUIRE(vfs.has_value());

        for (const auto& [path, expected] : extracted) {
            auto reader = (*vfs)->open(path);
            REQUIRE(reader.has_value());
            CHECK(verify_streaming_read(**reader, expected));
        }
    }

    TEST_CASE("ARC VFS") {
        auto archive_path = test::arc_dir() / "store.arc";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = arc_archive::open(archive_path);
        REQUIRE(archive.has_value());

        std::vector<std::pair<std::string, byte_vector>> extracted;
        for (const auto& file : (*archive)->files()) {
            auto data = (*archive)->extract(file);
            if (data.has_value()) {
                extracted.emplace_back(file.name, std::move(*data));
            }
        }

        archive = arc_archive::open(archive_path);
        auto vfs = archive_vfs::create(std::move(*archive));
        REQUIRE(vfs.has_value());

        for (const auto& [path, expected] : extracted) {
            auto reader = (*vfs)->open(path);
            REQUIRE(reader.has_value());
            CHECK(verify_streaming_read(**reader, expected));
        }
    }

    TEST_CASE("ZOO VFS") {
        auto archive_path = test::zoo_dir() / "default.zoo";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = zoo_archive::open(archive_path);
        REQUIRE(archive.has_value());

        std::vector<std::pair<std::string, byte_vector>> extracted;
        for (const auto& file : (*archive)->files()) {
            auto data = (*archive)->extract(file);
            if (data.has_value()) {
                extracted.emplace_back(file.name, std::move(*data));
            }
        }

        archive = zoo_archive::open(archive_path);
        auto vfs = archive_vfs::create(std::move(*archive));
        REQUIRE(vfs.has_value());

        for (const auto& [path, expected] : extracted) {
            auto reader = (*vfs)->open(path);
            REQUIRE(reader.has_value());
            CHECK(verify_streaming_read(**reader, expected));
        }
    }

    TEST_CASE("HA VFS") {
        auto archive_path = test::ha_dir() / "asc.ha";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = ha_archive::open(archive_path);
        REQUIRE(archive.has_value());

        std::vector<std::pair<std::string, byte_vector>> extracted;
        for (const auto& file : (*archive)->files()) {
            auto data = (*archive)->extract(file);
            if (data.has_value()) {
                extracted.emplace_back(file.name, std::move(*data));
            }
        }

        archive = ha_archive::open(archive_path);
        auto vfs = archive_vfs::create(std::move(*archive));
        REQUIRE(vfs.has_value());

        for (const auto& [path, expected] : extracted) {
            auto reader = (*vfs)->open(path);
            REQUIRE(reader.has_value());
            CHECK(verify_streaming_read(**reader, expected));
        }
    }

    TEST_CASE("HYP VFS") {
        auto archive_path = test::hyp_dir() / "license.hyp";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = hyp_archive::open(archive_path);
        REQUIRE(archive.has_value());

        std::vector<std::pair<std::string, byte_vector>> extracted;
        for (const auto& file : (*archive)->files()) {
            auto data = (*archive)->extract(file);
            if (data.has_value()) {
                extracted.emplace_back(file.name, std::move(*data));
            }
        }

        archive = hyp_archive::open(archive_path);
        auto vfs = archive_vfs::create(std::move(*archive));
        REQUIRE(vfs.has_value());

        for (const auto& [path, expected] : extracted) {
            auto reader = (*vfs)->open(path);
            REQUIRE(reader.has_value());
            CHECK(verify_streaming_read(**reader, expected));
        }
    }

    TEST_CASE("ACE VFS") {
        auto archive_path = test::ace_dir() / "license1.ace";
        REQUIRE(std::filesystem::exists(archive_path));

        auto archive = ace_archive::open(archive_path);
        REQUIRE(archive.has_value());

        std::vector<std::pair<std::string, byte_vector>> extracted;
        for (const auto& file : (*archive)->files()) {
            auto data = (*archive)->extract(file);
            if (data.has_value()) {
                extracted.emplace_back(file.name, std::move(*data));
            }
        }

        archive = ace_archive::open(archive_path);
        auto vfs = archive_vfs::create(std::move(*archive));
        REQUIRE(vfs.has_value());

        for (const auto& [path, expected] : extracted) {
            auto reader = (*vfs)->open(path);
            REQUIRE(reader.has_value());
            CHECK(verify_streaming_read(**reader, expected));
        }
    }
}

TEST_SUITE("open_archive - Auto-detection") {
    TEST_CASE("Detect RAR format") {
        auto archive_path = test::rar_dir() / "unrar_test_01.rar";
        REQUIRE(std::filesystem::exists(archive_path));

        auto data = read_file(archive_path);
        auto vfs = open_archive(data);
        REQUIRE(vfs.has_value());
        CHECK((*vfs)->file_count() > 0);
    }

    TEST_CASE("Detect CAB format") {
        auto cab_dir = test::cab_dir();
        REQUIRE(std::filesystem::exists(cab_dir));

        for (const auto& entry : std::filesystem::directory_iterator(cab_dir)) {
            if (entry.path().extension() != ".cab") continue;

            auto data = read_file(entry.path());
            auto vfs = open_archive(data);
            REQUIRE(vfs.has_value());
            CHECK((*vfs)->file_count() > 0);
            break;
        }
    }

    TEST_CASE("Detect LHA format") {
        auto lha_path = test::lha_dir() / "lha_unix114i" / "h1_lh5.lzh";
        REQUIRE(std::filesystem::exists(lha_path));

        auto data = read_file(lha_path);
        auto vfs = open_archive(data);
        REQUIRE(vfs.has_value());
        CHECK((*vfs)->file_count() > 0);
    }

    TEST_CASE("Detect ARJ format") {
        auto archive_path = test::arj_dir() / "method1.arj";
        REQUIRE(std::filesystem::exists(archive_path));

        auto data = read_file(archive_path);
        auto vfs = open_archive(data);
        REQUIRE(vfs.has_value());
        CHECK((*vfs)->file_count() > 0);
    }

    TEST_CASE("Detect ZOO format") {
        auto archive_path = test::zoo_dir() / "default.zoo";
        REQUIRE(std::filesystem::exists(archive_path));

        auto data = read_file(archive_path);
        auto vfs = open_archive(data);
        REQUIRE(vfs.has_value());
        CHECK((*vfs)->file_count() > 0);
    }

    TEST_CASE("Detect ARC format") {
        auto archive_path = test::arc_dir() / "store.arc";
        REQUIRE(std::filesystem::exists(archive_path));

        auto data = read_file(archive_path);
        auto vfs = open_archive(data);
        REQUIRE(vfs.has_value());
        CHECK((*vfs)->file_count() > 0);
    }

    TEST_CASE("Detect HA format") {
        auto archive_path = test::ha_dir() / "asc.ha";
        REQUIRE(std::filesystem::exists(archive_path));

        auto data = read_file(archive_path);
        auto vfs = open_archive(data);
        REQUIRE(vfs.has_value());
        CHECK((*vfs)->file_count() > 0);
    }

    TEST_CASE("Detect HYP format") {
        auto archive_path = test::hyp_dir() / "license.hyp";
        REQUIRE(std::filesystem::exists(archive_path));

        auto data = read_file(archive_path);
        auto vfs = open_archive(data);
        REQUIRE(vfs.has_value());
        CHECK((*vfs)->file_count() > 0);
    }

    TEST_CASE("Detect ACE format") {
        auto archive_path = test::ace_dir() / "license1.ace";
        REQUIRE(std::filesystem::exists(archive_path));

        auto data = read_file(archive_path);
        auto vfs = open_archive(data);
        REQUIRE(vfs.has_value());
        CHECK((*vfs)->file_count() > 0);
    }

    TEST_CASE("VFS extraction matches direct extraction") {
        // Verify that open_archive + VFS produces same results as direct extraction
        auto archive_path = test::lha_dir() / "lha_unix114i" / "h1_lh5.lzh";
        REQUIRE(std::filesystem::exists(archive_path));

        // Direct extraction
        auto archive = lha_archive::open(archive_path);
        REQUIRE(archive.has_value());
        std::vector<std::pair<std::string, byte_vector>> expected;
        for (const auto& file : (*archive)->files()) {
            auto data = (*archive)->extract(file);
            if (data.has_value()) {
                expected.emplace_back(file.name, std::move(*data));
            }
        }

        // Via open_archive + VFS
        auto file_data = read_file(archive_path);
        auto vfs = open_archive(file_data);
        REQUIRE(vfs.has_value());

        for (const auto& [path, exp_data] : expected) {
            auto vfs_data = (*vfs)->read(path);
            REQUIRE(vfs_data.has_value());
            CHECK(vfs_data->size() == exp_data.size());
            CHECK(std::memcmp(vfs_data->data(), exp_data.data(), exp_data.size()) == 0);
        }
    }
}
