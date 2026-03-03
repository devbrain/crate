#include <doctest/doctest.h>
#include <crate/formats/lha.hh>
#include <crate/test_config.hh>
#include <array>
#include <fstream>
#include <cstring>

using namespace crate;

namespace {
    const auto ARCHIVES_DIR = test::lha_dir();
    const auto OUTPUT_DIR = test::lha_dir() / "output";

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

    struct TestResult {
        int success = 0;
        int fail = 0;
        int unsupported = 0;
    };

    // Helper function to test an archive directory
    TestResult test_archive_directory(const std::string& dir_name, bool verify_content = true, [[maybe_unused]] bool quiet = false) {
        auto archive_dir = ARCHIVES_DIR / dir_name;
        auto output_dir_path = OUTPUT_DIR / dir_name;
        TestResult result;

        REQUIRE(std::filesystem::exists(archive_dir));

        for (const auto& entry : std::filesystem::recursive_directory_iterator(archive_dir)) {
            auto ext = entry.path().extension().string();
            if (ext != ".lzh" && ext != ".lha" && ext != ".lzs" && ext != ".pma") {
                continue;
            }

            auto archive = lha_archive::open(entry.path());
            if (!archive.has_value()) {
                result.fail++;
                continue;
            }

            auto& files = (*archive)->files();
            bool all_ok = true;

            for (const auto& file : files) {
                auto actual = (*archive)->extract(file);

                if (!actual.has_value()) {
                    if (actual.error().code() == error_code::UnsupportedCompression) {
                        result.unsupported++;
                    } else {
                        all_ok = false;
                    }
                    continue;
                }

                // Verify content if expected output exists
                if (verify_content) {
                    auto expected_path = output_dir_path / file.name;
                    if (std::filesystem::exists(expected_path) && std::filesystem::is_regular_file(expected_path)) {
                        auto expected = read_file(expected_path);
                        if (!compare_data(expected, *actual)) {
                            all_ok = false;
                        }
                    }
                }
            }

            if (all_ok) {
                result.success++;
            } else {
                result.fail++;
            }
        }

        return result;
    }
}

TEST_SUITE("LhaArchive - Basic") {
    TEST_CASE("Invalid data") {
        std::array<u8, 16> invalid_data = {0};
        auto archive = lha_archive::open(invalid_data);
        // May succeed with empty files list
        if (archive.has_value()) {
            CHECK((*archive)->files().empty());
        }
    }

    TEST_CASE("Empty data") {
        byte_span empty;
        auto archive = lha_archive::open(empty);
        if (archive.has_value()) {
            CHECK((*archive)->files().empty());
        }
    }
}

TEST_SUITE("LhaArchive - Archive Tests") {
    // Test cases using actual LHA archives from lhasa test suite

    TEST_CASE("lha_unix114i - Unix LHA archives") {
        auto result = test_archive_directory("lha_unix114i");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("lha213 - MS-DOS LHA 2.13") {
        auto result = test_archive_directory("lha213");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("lha255e - MS-DOS LHA 2.55e") {
        auto result = test_archive_directory("lha255e");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("larc333 - LArc 3.33") {
        auto result = test_archive_directory("larc333");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("lharc113 - LHarc 1.13") {
        auto result = test_archive_directory("lharc113");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("lharc_atari_313a - LHarc Atari 3.13a") {
        auto result = test_archive_directory("lharc_atari_313a");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("lhark04d - LHark 0.4d") {
        auto result = test_archive_directory("lhark04d");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("lha_amiga_122 - LHA Amiga 1.22") {
        auto result = test_archive_directory("lha_amiga_122");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("lha_amiga_212 - LHA Amiga 2.12") {
        auto result = test_archive_directory("lha_amiga_212");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("lha_os2_208 - LHA OS/2 2.08") {
        auto result = test_archive_directory("lha_os2_208");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("lha_os9_211c - LHA OS-9 2.11c") {
        auto result = test_archive_directory("lha_os9_211c");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("lha_osk_201 - LHA OS-9/68k 2.01") {
        auto result = test_archive_directory("lha_osk_201");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("lha_x68k_213 - LHA X68000 2.13") {
        auto result = test_archive_directory("lha_x68k_213");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("lhmelt_16536 - LHMelt 1.65.36") {
        auto result = test_archive_directory("lhmelt_16536");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("explzh_723 - Explzh 7.23") {
        auto result = test_archive_directory("explzh_723");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("unlha32 - UnLHA32") {
        auto result = test_archive_directory("unlha32");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("lh2_222 - LH2 2.22") {
        auto result = test_archive_directory("lh2_222");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("maclha_224 - MacLHA 2.24") {
        auto result = test_archive_directory("maclha_224");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("morphos_lha_2717 - MorphOS LHA 2.717") {
        auto result = test_archive_directory("morphos_lha_2717");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("tascal_lha_051h - Tascal LHA 0.51h") {
        auto result = test_archive_directory("tascal_lha_051h");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("pmarc124 - PMarc 1.24") {
        auto result = test_archive_directory("pmarc124");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("pmarc2 - PMarc 2") {
        auto result = test_archive_directory("pmarc2");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("lengths - Length edge cases") {
        auto result = test_archive_directory("lengths");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("generated - Generated test archives") {
        // Archives are in subdirectories (lzs/, pm1/)
        auto result = test_archive_directory("generated");
        CHECK(result.fail == 0);
        CHECK(result.success > 0);
    }

    TEST_CASE("regression - Regression tests") {
        // The regression directory contains intentionally broken archives
        // like truncated.lzh, which are expected to fail.
        // We just verify we don't crash on them.
        auto result = test_archive_directory("regression", true, true);
        // Some archives may parse (success) and some may fail — that's expected.
        // But we must have processed at least one archive.
        CHECK(result.success + result.fail > 0);
    }
}
