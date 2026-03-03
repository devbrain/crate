#include <doctest/doctest.h>
#include <crate/formats/floppy.hh>
#include <crate/test_config.hh>
#include <fstream>
#include <set>

using namespace crate;

namespace {
    const auto FLOPPY_DIR = test::floppy_dir();

    byte_vector read_file(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return byte_vector((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
    }
}

TEST_SUITE("FloppyImage - Basic") {
    TEST_CASE("Invalid data - too small") {
        std::array<u8, 256> small_data = {0};
        auto image = floppy_image::open(small_data);
        CHECK_FALSE(image.has_value());
    }

    TEST_CASE("Invalid data - bad boot sector") {
        std::array<u8, 512> invalid_data = {0};
        auto image = floppy_image::open(invalid_data);
        CHECK_FALSE(image.has_value());
    }
}

TEST_SUITE("FloppyImage - Turbo Pascal Disks") {
    TEST_CASE("Disk 1 - Install & Compiler") {
        auto image_path = FLOPPY_DIR / "Borland - Turbo Pascal v5.0 - Disk 1 of 3 - Install & Compiler.img";
        REQUIRE(std::filesystem::exists(image_path));

        auto data = read_file(image_path);
        REQUIRE(data.size() == 368640);  // 360KB floppy

        auto image = floppy_image::open(data);
        REQUIRE(image.has_value());

        const auto& files = (*image)->files();
        CHECK(files.size() > 0);

        MESSAGE("Found " << files.size() << " files:");
        std::set<std::string> filenames;
        for (const auto& f : files) {
            MESSAGE("  " << f.name << " (" << f.uncompressed_size << " bytes)");
            filenames.insert(f.name);
        }

        // Check for known Turbo Pascal files
        CHECK(filenames.count("INSTALL.EXE") > 0);
        CHECK(filenames.count("TPC.EXE") > 0);
    }

    TEST_CASE("Disk 2 - Help & Utilities") {
        auto image_path = FLOPPY_DIR / "Borland - Turbo Pascal v5.0 - Disk 2 of 3 - Help & Utilities.img";
        REQUIRE(std::filesystem::exists(image_path));

        auto data = read_file(image_path);
        REQUIRE(data.size() == 368640);

        auto image = floppy_image::open(data);
        REQUIRE(image.has_value());

        const auto& files = (*image)->files();
        CHECK(files.size() > 0);

        MESSAGE("Found " << files.size() << " files:");
        for (const auto& f : files) {
            MESSAGE("  " << f.name << " (" << f.uncompressed_size << " bytes)");
        }
    }

    TEST_CASE("Disk 3 - BGI, Demos, Doc") {
        auto image_path = FLOPPY_DIR / "Borland - Turbo Pascal v5.0 - Disk 3 of 3 - BGI, Demos, Doc, & Turbo3.img";
        REQUIRE(std::filesystem::exists(image_path));

        auto data = read_file(image_path);
        REQUIRE(data.size() == 368640);

        auto image = floppy_image::open(data);
        REQUIRE(image.has_value());

        const auto& files = (*image)->files();
        CHECK(files.size() > 0);

        MESSAGE("Found " << files.size() << " files:");
        for (const auto& f : files) {
            MESSAGE("  " << f.name << " (" << f.uncompressed_size << " bytes)");
        }
    }

    TEST_CASE("Extract file from Disk 1") {
        auto image_path = FLOPPY_DIR / "Borland - Turbo Pascal v5.0 - Disk 1 of 3 - Install & Compiler.img";
        REQUIRE(std::filesystem::exists(image_path));

        auto image = floppy_image::open(image_path);
        REQUIRE(image.has_value());

        // Find and extract a small file
        const file_entry* target = nullptr;
        for (const auto& f : (*image)->files()) {
            if (f.uncompressed_size > 0 && f.uncompressed_size < 10000) {
                target = &f;
                break;
            }
        }

        REQUIRE(target != nullptr);
        MESSAGE("Extracting: " << target->name << " (" << target->uncompressed_size << " bytes)");

        auto content = (*image)->extract(*target);
        REQUIRE(content.has_value());
        CHECK(content->size() == target->uncompressed_size);

        // Check that content is not all zeros
        bool all_zeros = true;
        for (u8 b : *content) {
            if (b != 0) {
                all_zeros = false;
                break;
            }
        }
        CHECK_FALSE(all_zeros);
    }

    TEST_CASE("Extract all files from Disk 1") {
        auto image_path = FLOPPY_DIR / "Borland - Turbo Pascal v5.0 - Disk 1 of 3 - Install & Compiler.img";
        REQUIRE(std::filesystem::exists(image_path));

        auto image = floppy_image::open(image_path);
        REQUIRE(image.has_value());

        size_t extracted = 0;
        size_t failed = 0;

        for (const auto& f : (*image)->files()) {
            auto content = (*image)->extract(f);
            if (content.has_value()) {
                CHECK(content->size() == f.uncompressed_size);
                extracted++;
            } else {
                MESSAGE("Failed to extract: " << f.name << " - " << content.error().message());
                failed++;
            }
        }

        MESSAGE("Extracted " << extracted << " files, " << failed << " failed");
        CHECK(failed == 0);
    }
}
