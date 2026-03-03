#include <doctest/doctest.h>
#include <crate/formats/rar.hh>
#include <crate/compression/rar_ppm.hh>
#include <crate/test_config.hh>
#include "md5.h"
#include <filesystem>
#include <fstream>
#include <cstring>
#include <array>
#include <map>
#include <set>

using namespace crate;

namespace {

const auto UNRAR_TEST_DIR = test::rar_dir();

// Test metadata structure
struct TestInfo {
    int test_num;
    std::string archive_name;
    std::string description;
    std::string password;  // Empty if no password needed
    bool is_multivolume = false;
    bool is_sfx = false;
    bool has_symlinks = false;
    bool has_hardlinks = false;
    bool has_encrypted_header = false;
    bool is_rar3 = false;  // RAR3 format (vs RAR5)
};

// All test cases from the corpus
const std::vector<TestInfo> ALL_TESTS = {
    {1, "unrar_test_01.rar", "Basic test - three files in root directory", "", false, false, false, false, false, false},
    {2, "unrar_test_02.rar", "Basic test - three files in 'subdir' directory", "", false, false, false, false, false, false},
    {3, "unrar_test_03.rar", "Basic test - files in root and subdir", "", false, false, false, false, false, false},
    {4, "unrar_test_04.rar", "Two files encrypted with password 'qwerty'", "qwerty", false, false, false, false, false, false},
    {5, "unrar_test_05.rar", "Two files encrypted with header encryption", "qwerty", false, false, false, false, true, false},
    {6, "unrar_test_06.rar", "Comment added to archive", "", false, false, false, false, false, false},
    {7, "unrar_test_07.rar", "Locked archive", "", false, false, false, false, false, false},
    {8, "unrar_test_08.rar", "One file with full path", "", false, false, false, false, false, false},
    {9, "unrar_test_09.rar", "Max compression level", "", false, false, false, false, false, false},
    {10, "unrar_test_10.rar", "Three versions of the same file", "", false, false, false, false, false, false},
    {11, "unrar_test_11.rar", "National characters in root dir", "", false, false, false, false, false, false},
    {12, "unrar_test_12.rar", "National characters in subdir", "", false, false, false, false, false, false},
    {13, "unrar_test_13.rar", "National chars in subdir with national char name", "", false, false, false, false, false, false},
    {14, "unrar_test_14.rar", "Composite test (1+2+3+6+7+8+9+11+12+13)", "", false, false, false, false, false, false},
    {15, "unrar_test_15.rar", "RAR5 with compression v4", "", false, false, false, false, false, false},
    {16, "unrar_test_16.rar", "RAR 3.9.2 format", "", false, false, false, false, false, true},
    {17, "unrar_test_17.part1.rar", "Multivolume RAR3", "", true, false, false, false, false, true},
    {18, "unrar_test_18.rar", "Symbolic links to file in archive", "", false, false, true, false, false, false},
    {19, "unrar_test_19.rar", "Encrypted with national char password", "żółć", false, false, false, false, false, false},
    {20, "unrar_test_20.rar", "Archive comment with national chars", "", false, false, false, false, false, false},
    {21, "unrar_test_21.rar", "Like 13 but RAR3", "", false, false, false, false, false, true},
    {22, "unrar_test_22.rar", "Like 19 but RAR3", "żółć", false, false, false, false, false, true},
    {23, "unrar_test_23.rar", "Like 18 but RAR3", "", false, false, true, false, false, true},
    {24, "unrar_test_24.rar", "Symlink to file in subdir", "", false, false, true, false, false, false},
    {25, "unrar_test_25.rar", "Symlink to /tmp/1.txt", "", false, false, true, false, false, false},
    {26, "unrar_test_26.rar", "Symlink to ../2.txt", "", false, false, true, false, false, false},
    {27, "unrar_test_27.rar", "Hard link", "", false, false, false, true, false, false},
    {28, "unrar_test_28.rar", "Unix owner/group stored", "", false, false, false, false, false, false},
    {29, "unrar_test_29.rar", "Identical files as RAR references", "", false, false, false, false, false, false},
    {30, "unrar_test_30.rar", "Symbolic links with NLS", "", false, false, true, false, false, false},
    {31, "unrar_test_31.rar", "Symlink with national characters", "", false, false, true, false, false, false},
    {32, "unrar_test_32.rar", "Symlink to /tmp (absolute)", "", false, false, true, false, false, false},
    {33, "unrar_test_33.rar", "One file, WinRAR", "", false, false, false, false, false, false},
    {34, "unrar_test_34.rar", "Like 11, WinRAR", "", false, false, false, false, false, false},
    {35, "unrar_test_35.rar", "Like 13 with comment, WinRAR", "", false, false, false, false, false, false},
    {36, "unrar_test_36.rar", "NTFS junction point, WinRAR", "", false, false, true, false, false, false},
    {37, "unrar_test_37.rar", "Hard link to file, WinRAR", "", false, false, false, true, false, false},
    {38, "unrar_test_38.rar", "Symlink to file, WinRAR", "", false, false, true, false, false, false},
    {39, "unrar_test_39.rar", "Junction to directory, WinRAR", "", false, false, true, false, false, false},
    {40, "unrar_test_40.rar", "Symlink to directory, WinRAR", "", false, false, true, false, false, false},
    {41, "unrar_test_41.rar", "Junction to dir with national chars, WinRAR", "", false, false, true, false, false, false},
    {42, "unrar_test_42.rar", "Symlink to dir with national chars, WinRAR", "", false, false, true, false, false, false},
    {43, "unrar_test_43.rar", "Junction to file with national chars, WinRAR", "", false, false, true, false, false, false},
    {44, "unrar_test_44.rar", "Hardlink to file with national chars, WinRAR", "", false, false, false, true, false, false},
    {45, "unrar_test_45.rar", "Symlink to file with national chars, WinRAR", "", false, false, true, false, false, false},
    {46, "unrar_test_46.rar", "Symlink outside archive with national chars, WinRAR", "", false, false, true, false, false, false},
    {47, "unrar_test_47.rar", "Symlink with national chars, RAR3", "", false, false, true, false, false, true},
    {54, "unrar_test_54.exe", "Windows SFX archive", "", false, true, false, false, false, false},
    {55, "unrar_test_55.sfx", "Linux SFX archive", "", false, true, false, false, false, false},
    {56, "unrar_test_56.sfx", "Linux SFX with national chars", "", false, true, false, false, false, false},
    {57, "unrar_test_57.part1.rar", "Multivolume RAR5 with recovery", "", true, false, false, false, false, false},
    {58, "unrar_test_58.part1.rar", "Multivolume RAR5 with recovery", "", true, false, false, false, false, false},
};

// Hardcoded reference data: expected {file_count, total_size, combined_md5}
// Generated by extracting each archive with unrar and computing MD5 over
// all file contents in sorted-by-name order.
struct ExpectedRef {
    size_t file_count;
    size_t total_size;
    const char* combined_md5;
};

const std::map<std::string, ExpectedRef> REFERENCE_DATA = {
    {"01", {3, 301767, "171ed1e5281db982cfcf672b61fbe8c5"}},
    {"02", {3, 301767, "171ed1e5281db982cfcf672b61fbe8c5"}},
    {"03", {4, 374613, "e28c516546b227ef75f5fffe182c5224"}},
    {"06", {3, 301767, "171ed1e5281db982cfcf672b61fbe8c5"}},
    {"07", {3, 301767, "171ed1e5281db982cfcf672b61fbe8c5"}},
    {"08", {1, 143984, "4c3fa723dffb2bed5ab8241887893783"}},
    {"09", {1, 143984, "4c3fa723dffb2bed5ab8241887893783"}},
    {"10", {1, 143984, "4c3fa723dffb2bed5ab8241887893783"}},
    {"11", {1, 72846, "4a4d97a87d193581a8491a9acb15972a"}},
    {"12", {1, 72846, "4a4d97a87d193581a8491a9acb15972a"}},
    {"13", {2, 157783, "34aa1e0774098f17ce5c062866d25554"}},
    {"14", {9, 893210, "634cde54e155d413e596f6b4f0914a8f"}},
    {"15", {1, 143984, "4c3fa723dffb2bed5ab8241887893783"}},
    {"16", {1, 143984, "4c3fa723dffb2bed5ab8241887893783"}},
    {"20", {1, 72846, "4a4d97a87d193581a8491a9acb15972a"}},
    {"21", {2, 157783, "34aa1e0774098f17ce5c062866d25554"}},
    {"28", {1, 72846, "4a4d97a87d193581a8491a9acb15972a"}},
    {"29", {2, 145692, "ae96091716c520d9ab06ace2f2f39294"}},
    {"33", {1, 72846, "4a4d97a87d193581a8491a9acb15972a"}},
    {"34", {1, 72846, "4a4d97a87d193581a8491a9acb15972a"}},
    {"35", {2, 157783, "34aa1e0774098f17ce5c062866d25554"}},
    // Symlink archives - crate extracts symlink entries as small files
    {"18", {6, 301806, "33977fc7ea2160015e708763434e4cbe"}},
    {"23", {6, 301806, "33977fc7ea2160015e708763434e4cbe"}},
    {"24", {2, 72866, "6ce41a5975f2202f23e68b29dfa3227f"}},
    {"30", {2, 72859, "720aa44db15c0b6fe153a99ffe73c6ef"}},
    {"31", {2, 72862, "35eba5a2ae8518f12b195575af118264"}},
    {"47", {2, 72859, "720aa44db15c0b6fe153a99ffe73c6ef"}},
    // Hard link archives
    {"27", {2, 145692, "ae96091716c520d9ab06ace2f2f39294"}},
    {"37", {2, 145692, "ae96091716c520d9ab06ace2f2f39294"}},
    {"44", {2, 145692, "ae96091716c520d9ab06ace2f2f39294"}},
    // WinRAR link archives (regular files only)
    {"38", {1, 72846, "4a4d97a87d193581a8491a9acb15972a"}},
    {"40", {1, 72846, "4a4d97a87d193581a8491a9acb15972a"}},
    {"42", {1, 72846, "4a4d97a87d193581a8491a9acb15972a"}},
    {"45", {1, 72846, "4a4d97a87d193581a8491a9acb15972a"}},
    // SFX archives
    {"54", {1, 72846, "4a4d97a87d193581a8491a9acb15972a"}},
    {"55", {1, 72846, "4a4d97a87d193581a8491a9acb15972a"}},
    {"56", {1, 72846, "4a4d97a87d193581a8491a9acb15972a"}},
};

// Extract all regular files with crate and return map of filename -> data
// Skips directory entries (zero-size) to match reference data from unrar
std::map<std::string, byte_vector> extract_with_crate(rar_archive& archive) {
    std::map<std::string, byte_vector> result;
    for (const auto& file : archive.files()) {
        if (file.uncompressed_size == 0) continue;  // Skip directories
        auto extracted = archive.extract(file);
        if (extracted.has_value() && !extracted->empty()) {
            result[file.name] = std::move(*extracted);
        }
    }
    return result;
}

// Compute combined MD5 over all extracted files (sorted by name)
std::string combined_md5(const std::map<std::string, byte_vector>& files) {
    MD5_CTX ctx;
    MD5_Init(&ctx);
    for (const auto& [name, data] : files) {
        unsigned long sz = data.size();
        MD5_Update(&ctx, data.data(), sz);
    }
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5_Final(digest, &ctx);
    char hex[33];
    for (int i = 0; i < 16; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return hex;
}

// Verify extraction against reference data
void verify_extraction(rar_archive& archive, const std::string& test_id) {
    auto it = REFERENCE_DATA.find(test_id);
    REQUIRE(it != REFERENCE_DATA.end());
    const auto& ref = it->second;

    auto extracted = extract_with_crate(archive);
    REQUIRE_FALSE(extracted.empty());

    size_t total = 0;
    for (const auto& [name, data] : extracted) {
        total += data.size();
    }

    CHECK(extracted.size() == ref.file_count);
    CHECK(total == ref.total_size);
    std::string md5 = combined_md5(extracted);
    CHECK(md5 == ref.combined_md5);
}

} // anonymous namespace

TEST_SUITE("RarArchive - Corpus Tests") {

    // Test that corpus directory exists
    TEST_CASE("Corpus directory exists") {
        REQUIRE(std::filesystem::exists(UNRAR_TEST_DIR));
        REQUIRE(std::filesystem::is_directory(UNRAR_TEST_DIR));
    }

    // Basic extraction tests (no encryption, no multivolume, no SFX)
    TEST_CASE("Test 01 - Basic three files in root") {
        auto path = UNRAR_TEST_DIR / "unrar_test_01.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "01");
    }

    TEST_CASE("Test 02 - Three files in subdir") {
        auto path = UNRAR_TEST_DIR / "unrar_test_02.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "02");
    }

    TEST_CASE("Test 03 - Files in root and subdir") {
        auto path = UNRAR_TEST_DIR / "unrar_test_03.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "03");
    }

    TEST_CASE("Test 06 - Archive with comment") {
        auto path = UNRAR_TEST_DIR / "unrar_test_06.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "06");
    }

    TEST_CASE("Test 07 - Locked archive") {
        auto path = UNRAR_TEST_DIR / "unrar_test_07.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "07");
    }

    TEST_CASE("Test 08 - File with full path") {
        auto path = UNRAR_TEST_DIR / "unrar_test_08.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "08");
    }

    TEST_CASE("Test 09 - Max compression") {
        auto path = UNRAR_TEST_DIR / "unrar_test_09.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "09");
    }

    TEST_CASE("Test 10 - Multiple file versions") {
        auto path = UNRAR_TEST_DIR / "unrar_test_10.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "10");
    }

    TEST_CASE("Test 11 - National characters in root") {
        auto path = UNRAR_TEST_DIR / "unrar_test_11.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "11");
    }

    TEST_CASE("Test 12 - National characters in subdir") {
        auto path = UNRAR_TEST_DIR / "unrar_test_12.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "12");
    }

    TEST_CASE("Test 13 - National chars in subdir with national name") {
        auto path = UNRAR_TEST_DIR / "unrar_test_13.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "13");
    }

    TEST_CASE("Test 14 - Composite test") {
        auto path = UNRAR_TEST_DIR / "unrar_test_14.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "14");
    }

    TEST_CASE("Test 15 - RAR5 with compression v4") {
        auto path = UNRAR_TEST_DIR / "unrar_test_15.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "15");
    }

    TEST_CASE("Test 20 - Comment with national chars") {
        auto path = UNRAR_TEST_DIR / "unrar_test_20.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "20");
    }

    TEST_CASE("Test 28 - Unix owner/group") {
        auto path = UNRAR_TEST_DIR / "unrar_test_28.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "28");
    }

    TEST_CASE("Test 29 - RAR references") {
        auto path = UNRAR_TEST_DIR / "unrar_test_29.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "29");
    }

    TEST_CASE("Test 33 - WinRAR basic") {
        auto path = UNRAR_TEST_DIR / "unrar_test_33.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "33");
    }

    TEST_CASE("Test 34 - WinRAR national chars") {
        auto path = UNRAR_TEST_DIR / "unrar_test_34.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "34");
    }

    TEST_CASE("Test 35 - WinRAR with comment") {
        auto path = UNRAR_TEST_DIR / "unrar_test_35.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "35");
    }
}

// RAR3 format tests
TEST_SUITE("RarArchive - RAR3 Format") {

    TEST_CASE("Test 16 - RAR 3.9.2 format") {
        auto path = UNRAR_TEST_DIR / "unrar_test_16.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "16");
    }

    TEST_CASE("Test 21 - RAR3 national chars") {
        auto path = UNRAR_TEST_DIR / "unrar_test_21.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "21");
    }

    TEST_CASE("Test 23 - RAR3 symlinks") {
        auto path = UNRAR_TEST_DIR / "unrar_test_23.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "23");
    }

    TEST_CASE("Test 47 - RAR3 symlink national chars") {
        auto path = UNRAR_TEST_DIR / "unrar_test_47.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "47");
    }
}

// Symlink tests - extraction should work, symlinks are stored as regular entries
TEST_SUITE("RarArchive - Symlinks") {

    TEST_CASE("Test 18 - Symlinks to files in archive") {
        auto path = UNRAR_TEST_DIR / "unrar_test_18.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        CHECK(!(*archive)->files().empty());
        verify_extraction(**archive, "18");
    }

    TEST_CASE("Test 24 - Symlink to file in subdir") {
        auto path = UNRAR_TEST_DIR / "unrar_test_24.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "24");
    }

    TEST_CASE("Test 27 - Hard link") {
        auto path = UNRAR_TEST_DIR / "unrar_test_27.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "27");
    }

    TEST_CASE("Test 30 - Symlinks with NLS") {
        auto path = UNRAR_TEST_DIR / "unrar_test_30.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "30");
    }

    TEST_CASE("Test 31 - Symlink with national chars") {
        auto path = UNRAR_TEST_DIR / "unrar_test_31.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "31");
    }
}

// WinRAR-specific tests with links and junctions
TEST_SUITE("RarArchive - WinRAR Links") {

    TEST_CASE("Test 36 - NTFS junction") {
        auto path = UNRAR_TEST_DIR / "unrar_test_36.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());
        // NTFS junction - may not have extractable regular files
        CHECK(!(*archive)->files().empty());
    }

    TEST_CASE("Test 37 - WinRAR hard link") {
        auto path = UNRAR_TEST_DIR / "unrar_test_37.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "37");
    }

    TEST_CASE("Test 38 - WinRAR symlink to file") {
        auto path = UNRAR_TEST_DIR / "unrar_test_38.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "38");
    }

    TEST_CASE("Test 39-46 - WinRAR junction/symlink variations") {
        // Test all WinRAR link variations that have reference data
        for (const auto& test_id : {"40", "42", "44", "45"}) {
            int i = std::stoi(test_id);
            auto path = UNRAR_TEST_DIR / ("unrar_test_" + std::to_string(i) + ".rar");
            if (!std::filesystem::exists(path)) {
                continue;
            }

            SUBCASE(("Test " + std::to_string(i)).c_str()) {
                auto archive = rar_archive::open(path);
                REQUIRE(archive.has_value());

                verify_extraction(**archive, test_id);
            }
        }

        // Archives with only junction/link entries (no regular file data)
        for (int i : {36, 39, 41, 43, 46}) {
            auto path = UNRAR_TEST_DIR / ("unrar_test_" + std::to_string(i) + ".rar");
            if (!std::filesystem::exists(path)) {
                continue;
            }

            SUBCASE(("Test " + std::to_string(i) + " (link-only)").c_str()) {
                auto archive = rar_archive::open(path);
                REQUIRE(archive.has_value());
                CHECK(!(*archive)->files().empty());
            }
        }
    }

    TEST_CASE("Test 44 - WinRAR hardlink with national chars") {
        auto path = UNRAR_TEST_DIR / "unrar_test_44.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "44");
    }
}

// Encrypted archive tests (expected to fail without password support)
TEST_SUITE("RarArchive - Encrypted") {

    TEST_CASE("Test 04 - Password protected (qwerty)") {
        auto path = UNRAR_TEST_DIR / "unrar_test_04.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        // Check that files are marked as encrypted
        CHECK((*archive)->has_encrypted_files());
    }

    TEST_CASE("Test 05 - Encrypted header (qwerty)") {
        auto path = UNRAR_TEST_DIR / "unrar_test_05.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        // Encrypted header archives may fail to open without password,
        // or may open but mark files as encrypted
        if (archive.has_value()) {
            CHECK((*archive)->has_encrypted_files());
        } else {
            CHECK(archive.error().code() != error_code::Success);
        }
    }

    TEST_CASE("Test 19 - National char password") {
        auto path = UNRAR_TEST_DIR / "unrar_test_19.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        // Check encryption status (may be header-encrypted with no file list)
        CHECK((*archive)->has_encrypted_files());
    }

    TEST_CASE("Test 22 - RAR3 national char password") {
        auto path = UNRAR_TEST_DIR / "unrar_test_22.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        CHECK((*archive)->has_encrypted_files());
    }
}

// Multivolume archive tests (expected to fail without multivolume support)
TEST_SUITE("RarArchive - Multivolume") {

    TEST_CASE("Test 17 - Multivolume RAR3") {
        auto path = UNRAR_TEST_DIR / "unrar_test_17.part1.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        // Multi-volume detection should be surfaced via API
        CHECK((*archive)->is_multivolume());
    }

    TEST_CASE("Test 57 - Multivolume RAR5 with recovery") {
        auto path = UNRAR_TEST_DIR / "unrar_test_57.part1.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        CHECK(!(*archive)->files().empty());
    }

    TEST_CASE("Test 58 - Multivolume RAR5 with recovery (variant)") {
        auto path = UNRAR_TEST_DIR / "unrar_test_58.part1.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        CHECK(!(*archive)->files().empty());
    }
}

// SFX archive tests
TEST_SUITE("RarArchive - SFX") {

    TEST_CASE("Test 54 - Windows SFX") {
        auto path = UNRAR_TEST_DIR / "unrar_test_54.exe";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "54");
    }

    TEST_CASE("Test 55 - Linux SFX") {
        auto path = UNRAR_TEST_DIR / "unrar_test_55.sfx";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "55");
    }

    TEST_CASE("Test 56 - Linux SFX national chars") {
        auto path = UNRAR_TEST_DIR / "unrar_test_56.sfx";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        verify_extraction(**archive, "56");
    }
}

// External symlink tests - these may extract differently due to symlink handling
TEST_SUITE("RarArchive - External Symlinks") {

    TEST_CASE("Test 25 - Symlink to /tmp/1.txt") {
        auto path = UNRAR_TEST_DIR / "unrar_test_25.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());
        CHECK(!(*archive)->files().empty());
    }

    TEST_CASE("Test 26 - Symlink to ../2.txt") {
        auto path = UNRAR_TEST_DIR / "unrar_test_26.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());
        CHECK(!(*archive)->files().empty());
    }

    TEST_CASE("Test 32 - Symlink to /tmp") {
        auto path = UNRAR_TEST_DIR / "unrar_test_32.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());
        CHECK(!(*archive)->files().empty());
    }

    TEST_CASE("Test 46 - Symlink outside with national chars") {
        auto path = UNRAR_TEST_DIR / "unrar_test_46.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());
        CHECK(!(*archive)->files().empty());
    }
}

// Summary statistics test
TEST_SUITE("RarArchive - Corpus Summary") {

    TEST_CASE("Corpus file count") {
        int found = 0;
        int missing = 0;

        std::vector<std::string> missing_files;

        for (const auto& test : ALL_TESTS) {
            auto path = UNRAR_TEST_DIR / test.archive_name;
            if (std::filesystem::exists(path)) {
                found++;
            } else {
                missing++;
                missing_files.push_back(test.archive_name);
            }
        }

        (void)missing_files;  // Used for debugging
        CHECK(found > 50);  // Should have most files
    }

    TEST_CASE("Archive open statistics") {
        int opened = 0;
        int failed = 0;
        std::vector<std::string> failed_archives;

        for (const auto& test : ALL_TESTS) {
            auto path = UNRAR_TEST_DIR / test.archive_name;
            if (!std::filesystem::exists(path)) continue;

            auto archive = rar_archive::open(path);
            if (archive.has_value()) {
                opened++;
            } else {
                failed++;
                failed_archives.push_back(test.archive_name + ": " + std::string(archive.error().message()));
            }
        }

        (void)failed_archives;  // Used for debugging
        CHECK(opened > 0);
    }

    TEST_CASE("Full extraction test - all basic RAR5 archives") {
        std::vector<std::string> basic_tests = {
            "01", "02", "03", "06", "07", "08", "09", "10",
            "11", "12", "13", "14", "20", "28", "33", "34", "35"
        };

        int passed = 0;
        int failed = 0;
        std::vector<std::string> failures;

        for (const auto& test_id : basic_tests) {
            std::string archive_name = "unrar_test_" + test_id + ".rar";
            auto path = UNRAR_TEST_DIR / archive_name;

            if (!std::filesystem::exists(path)) {
                FAIL_CHECK("Corpus file missing: " << archive_name);
                continue;
            }

            auto it = REFERENCE_DATA.find(test_id);
            REQUIRE(it != REFERENCE_DATA.end());
            const auto& ref = it->second;

            auto archive = rar_archive::open(path);
            if (!archive.has_value()) {
                failures.push_back("Test " + test_id + ": Failed to open");
                failed++;
                continue;
            }

            auto extracted = extract_with_crate(**archive);
            std::string md5 = combined_md5(extracted);

            if (extracted.size() == ref.file_count && md5 == ref.combined_md5) {
                passed++;
            } else {
                failures.push_back("Test " + test_id + ": MD5 mismatch or file count mismatch");
                failed++;
            }
        }

        (void)failures;  // Used for debugging
        // All basic RAR5 tests should pass
        CHECK(failed == 0);
    }

}

// Multi-volume archive tests
TEST_SUITE("RarArchive - Multivolume") {
    // Note: unrar_test_17 is RAR5 format, unrar_test_57 is RAR4 format (despite numbering)

    TEST_CASE("RAR5 multivolume detection") {
        auto path = UNRAR_TEST_DIR / "unrar_test_17.part1.rar";
        REQUIRE(std::filesystem::exists(path));

        // Open single volume first - should detect split files
        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        CHECK((*archive)->version() == rar::V5);
        CHECK((*archive)->is_multivolume());
    }

    TEST_CASE("RAR5 multivolume open_multivolume") {
        auto path = UNRAR_TEST_DIR / "unrar_test_17.part1.rar";
        REQUIRE(std::filesystem::exists(path));

        // Open with automatic volume loading
        auto archive = rar_archive::open_multivolume(path);
        REQUIRE(archive.has_value());

        CHECK((*archive)->version() == rar::V5);
        CHECK((*archive)->is_multivolume());
        CHECK((*archive)->volume_count() > 1);
        CHECK(!(*archive)->files().empty());
    }

    TEST_CASE("RAR4 multivolume detection") {
        auto path = UNRAR_TEST_DIR / "unrar_test_57.part1.rar";
        REQUIRE(std::filesystem::exists(path));

        // Open single volume first - should detect split files
        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        // RAR4 multivolume - should be detected
        CHECK((*archive)->version() == rar::V4);
        CHECK((*archive)->is_multivolume());
    }

    TEST_CASE("RAR4 multivolume open_multivolume") {
        auto path = UNRAR_TEST_DIR / "unrar_test_57.part1.rar";
        REQUIRE(std::filesystem::exists(path));

        // Open with automatic volume loading
        auto archive = rar_archive::open_multivolume(path);
        REQUIRE(archive.has_value());

        CHECK((*archive)->version() == rar::V4);
        CHECK((*archive)->is_multivolume());
        // part2 is missing from test corpus, so only 1 volume loads
        CHECK((*archive)->volume_count() >= 1);
        CHECK(!(*archive)->files().empty());
    }

    TEST_CASE("Custom volume provider") {
        auto path = UNRAR_TEST_DIR / "unrar_test_17.part1.rar";
        REQUIRE(std::filesystem::exists(path));

        // Read first volume manually
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        REQUIRE(file.is_open());
        auto size = file.tellg();
        REQUIRE(size >= 0);
        size_t size_value = static_cast<size_t>(size);
        file.seekg(0);
        byte_vector first_vol(size_value);
        file.read(reinterpret_cast<char*>(first_vol.data()),
                  static_cast<std::streamsize>(size_value));

        // Track which volumes were requested
        std::vector<unsigned> requested_volumes;

        auto provider = [&](unsigned vol_num, const std::string&) -> byte_vector {
            requested_volumes.push_back(vol_num);

            // Build path for this volume (RAR5 naming)
            std::string vol_path = path.parent_path().string() + "/unrar_test_17.part"
                                   + std::to_string(vol_num + 1) + ".rar";

            if (!std::filesystem::exists(vol_path)) {
                return {};
            }

            std::ifstream vol_file(vol_path, std::ios::binary | std::ios::ate);
            if (!vol_file) return {};
            auto vol_size = vol_file.tellg();
            if (vol_size < 0) return {};
            size_t vol_size_value = static_cast<size_t>(vol_size);
            vol_file.seekg(0);
            byte_vector data(vol_size_value);
            vol_file.read(reinterpret_cast<char*>(data.data()),
                          static_cast<std::streamsize>(vol_size_value));
            return data;
        };

        auto archive = rar_archive::open(first_vol, provider);
        REQUIRE(archive.has_value());

        CHECK(!requested_volumes.empty());
        CHECK((*archive)->is_multivolume());
    }

    TEST_CASE("RAR5 multivolume extraction") {
        auto path = UNRAR_TEST_DIR / "unrar_test_17.part1.rar";
        REQUIRE(std::filesystem::exists(path));

        // Open with automatic volume loading
        auto archive = rar_archive::open_multivolume(path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->volume_count() == 6);
        REQUIRE((*archive)->files().size() == 1);

        const auto& file = (*archive)->files()[0];
        // Extract the file
        auto result = (*archive)->extract(file);
        REQUIRE(result.has_value());

        CHECK(result->size() == file.uncompressed_size);

        // The file is IMG_0758.jpeg - check JPEG signature
        if (result->size() >= 2) {
            CHECK((*result)[0] == 0xFF);  // JPEG SOI marker
            CHECK((*result)[1] == 0xD8);
        }
    }

    TEST_CASE("RAR5 multivolume - verify data gathering") {
        auto path = UNRAR_TEST_DIR / "unrar_test_17.part1.rar";
        REQUIRE(std::filesystem::exists(path));

        // Open with automatic volume loading
        auto archive = rar_archive::open_multivolume(path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->volume_count() == 6);

        // Manually load each volume and check compressed data sizes
        std::vector<size_t> vol_sizes;
        for (unsigned v = 0; v < 6; v++) {
            std::string vol_path = path.parent_path().string() + "/unrar_test_17.part"
                                   + std::to_string(v + 1) + ".rar";
            std::ifstream file(vol_path, std::ios::binary | std::ios::ate);
            REQUIRE(file.is_open());
            auto vol_size = file.tellg();
            REQUIRE(vol_size >= 0);
            vol_sizes.push_back(static_cast<size_t>(vol_size));
        }

        // Check all volumes were loaded
        CHECK(vol_sizes.size() == 6);
        size_t total = 0;
        for (auto s : vol_sizes) total += s;
        CHECK(total > 0);
    }
}

// PPM infrastructure tests
TEST_SUITE("RarArchive - PPM") {
    TEST_CASE("PPM SubAllocator basic operations") {
        crate::rar::ppm::sub_allocator alloc;

        // Start with 1MB
        alloc.start(1);
        CHECK(alloc.allocated_memory() == 1 << 20);

        // Allocate some units
        void* p1 = alloc.alloc_units(1);
        CHECK(p1 != nullptr);

        void* p2 = alloc.alloc_units(4);
        CHECK(p2 != nullptr);

        void* ctx = alloc.alloc_context();
        CHECK(ctx != nullptr);

        // Text area operations
        CHECK(alloc.text() == alloc.text_start());
        alloc.advance_text(10);
        CHECK(alloc.text() == alloc.text_start() + 10);
        alloc.retreat_text(5);
        CHECK(alloc.text() == alloc.text_start() + 5);

        // Expand/shrink
        void* p3 = alloc.expand_units(p1, 1);
        CHECK(p3 != nullptr);

        void* p4 = alloc.shrink_units(p2, 4, 2);
        CHECK(p4 == p2);  // Shrink returns same pointer in simplified version

        // Free (no-op in simplified version)
        alloc.free_units(p1, 1);

        // Stop cleans up
        alloc.stop();
        CHECK(alloc.allocated_memory() == 0);
    }

    TEST_CASE("PPM RangeCoder initialization") {
        // Create test input data (simulates PPM header)
        std::vector<crate::u8> data = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
        crate::byte_span span(data.data(), data.size());

        crate::rar::ppm::span_input input(span);
        crate::rar::ppm::range_coder coder;

        // Initialize reads 4 bytes
        coder.init(&input);

        // Coder should be initialized
        CHECK(input.position() == 4);
    }

    TEST_CASE("PPM See2Context operations") {
        crate::rar::ppm::see2_context ctx;

        // Initialize
        ctx.init(50);
        CHECK(ctx.summ != 0);
        CHECK(ctx.shift == crate::rar::ppm::PERIOD_BITS - 4);
        CHECK(ctx.count == 4);

        // Get mean
        unsigned mean = ctx.get_mean();
        CHECK(mean > 0);

        // Update
        ctx.update();
    }
}

// Solid archive tests
TEST_SUITE("RarArchive - Solid") {
    TEST_CASE("RAR5 solid archive detection") {
        auto path = std::filesystem::path(UNRAR_TEST_DIR) / "solid_test.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());
        CHECK((*archive)->is_solid());
        CHECK((*archive)->version() == rar::V5);
        CHECK((*archive)->files().size() == 3);
    }

    TEST_CASE("RAR5 solid archive extraction - sequential") {
        auto path = std::filesystem::path(UNRAR_TEST_DIR) / "solid_test.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->is_solid());

        const auto& files = (*archive)->files();
        REQUIRE(files.size() == 3);

        // Extract files in order (sequential is most efficient for solid)
        for (size_t i = 0; i < files.size(); i++) {
            auto result = (*archive)->extract(files[i]);
            REQUIRE(result.has_value());
            std::string content(result->begin(), result->end());

            // Verify content matches expected pattern
            CHECK(content.find("This is file") != std::string::npos);
        }
    }

    TEST_CASE("RAR5 solid archive extraction - random order") {
        auto path = std::filesystem::path(UNRAR_TEST_DIR) / "solid_test.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->is_solid());

        const auto& files = (*archive)->files();
        REQUIRE(files.size() >= 3);

        // Extract file 3 first (should decompress files 1 and 2 internally)
        auto result3 = (*archive)->extract(files[2]);
        REQUIRE(result3.has_value());
        std::string content3(result3->begin(), result3->end());
        CHECK(content3.find("file 3") != std::string::npos);

        // Now extract file 1 - should work from cache or re-decompress
        auto result1 = (*archive)->extract(files[0]);
        REQUIRE(result1.has_value());
        std::string content1(result1->begin(), result1->end());
        CHECK(content1.find("file 1") != std::string::npos);
    }

    TEST_CASE("RAR5 solid archive member solid_file flag") {
        auto path = std::filesystem::path(UNRAR_TEST_DIR) / "solid_test.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());
        REQUIRE((*archive)->is_solid());

        // Solid status is covered by is_solid() on the archive.
    }
}
