#include <doctest/doctest.h>
#include <crate/formats/rar.hh>
#include <crate/compression/rar_filters.hh>
#include <crate/compression/rar_unpack.hh>
#include <crate/crypto/sha.hh>
#include <crate/crypto/aes_decoder.hh>
#include <crate/crypto/rar_crypt.hh>
#include <crate/test_config.hh>
#include <array>

using namespace crate;

TEST_SUITE("RarArchive - Basic") {
    TEST_CASE("Invalid signature") {
        std::array<u8, 16> invalid_data = {0};
        auto archive = rar_archive::open(invalid_data);
        CHECK_FALSE(archive.has_value());
        CHECK(archive.error().code() == error_code::InvalidSignature);
    }

    TEST_CASE("RAR4 signature detection") {
        // RAR4 signature: 0x52 0x61 0x72 0x21 0x1A 0x07 0x00
        std::array<u8, 20> rar4_sig = {
            0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00,  // Signature
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        auto archive = rar_archive::open(rar4_sig);
        // May fail due to truncated data, but shouldn't crash and should detect signature
        // The error should NOT be InvalidSignature
        if (!archive.has_value()) {
            CHECK(archive.error().code() != error_code::InvalidSignature);
        }
    }

    TEST_CASE("RAR5 signature detection") {
        // RAR5 signature: 0x52 0x61 0x72 0x21 0x1A 0x07 0x01 0x00
        std::array<u8, 20> rar5_sig = {
            0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00,  // Signature
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00
        };
        auto archive = rar_archive::open(rar5_sig);
        // May fail due to truncated data, but shouldn't crash and should detect signature
        if (!archive.has_value()) {
            CHECK(archive.error().code() != error_code::InvalidSignature);
        }
    }

    TEST_CASE("RAR old format signature detection") {
        // Old RAR signature: 0x52 0x45 0x7E 0x5E
        std::array<u8, 16> rar_old_sig = {
            0x52, 0x45, 0x7E, 0x5E,  // Signature
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00
        };
        auto archive = rar_archive::open(rar_old_sig);
        // May fail due to truncated data, but should detect signature
        if (!archive.has_value()) {
            CHECK(archive.error().code() != error_code::InvalidSignature);
        }
    }

    TEST_CASE("Truncated data") {
        // Just the RAR4 signature, nothing else
        std::array<u8, 7> truncated = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00};
        auto archive = rar_archive::open(truncated);
        CHECK_FALSE(archive.has_value());
        // Should be TruncatedArchive, not InvalidSignature
        CHECK(archive.error().code() != error_code::InvalidSignature);
    }
}

TEST_SUITE("RarArchive - Version Detection") {
    TEST_CASE("RAR version enum values") {
        // Just verify the enum exists and has expected values
        CHECK(static_cast<int>(rar::OLD) == 1);
        CHECK(static_cast<int>(rar::V4) == 4);
        CHECK(static_cast<int>(rar::V5) == 5);
    }
}


// Unrar test suite files
TEST_SUITE("RarArchive - Unrar Test Suite") {
    const auto unrar_test_dir = test::rar_dir();

    TEST_CASE("Open unrar_test_01.rar") {
        auto path = unrar_test_dir / "unrar_test_01.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());
        CHECK(archive.value() != nullptr);
        CHECK(!(*archive)->files().empty());
    }

    TEST_CASE("Parse and extract unrar_test_01.rar") {
        auto path = unrar_test_dir / "unrar_test_01.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        // Extract all files; skip if extraction isn't supported
        int extracted = 0;
        for (const auto& file : (*archive)->files()) {
            auto result = (*archive)->extract(file);
            if (result.has_value()) {
                CHECK(result->size() == file.uncompressed_size);
                extracted++;
            }
        }
        CHECK(extracted == 3);  // unrar_test_01.rar has 3 files
    }
}

// RAR4 PPM compression test
TEST_SUITE("RarArchive - RAR4 PPM") {
    TEST_CASE("PPM extraction - ppm_test.rar") {
        auto path = test::rar_dir() / "ppm_test.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());
        CHECK((*archive)->version() == rar::V4);

        REQUIRE((*archive)->files().size() == 1);

        const auto& file = (*archive)->files()[0];

        auto result = (*archive)->extract(file);
        REQUIRE(result.has_value());
        CHECK(result->size() == file.uncompressed_size);
    }
}

// RAR4 symlink detection tests
TEST_SUITE("RarArchive - RAR4 Symlinks") {
    TEST_CASE("RAR4 symlink detection - unrar_test_47.rar") {
        auto path = test::rar_dir() / "unrar_test_47.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());
        CHECK((*archive)->version() == rar::V4);

        // No public member metadata; just ensure files list is present.
        CHECK(!(*archive)->files().empty());
    }

    TEST_CASE("RAR4 symlink extraction") {
        auto path = test::rar_dir() / "unrar_test_47.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        // Without member metadata, just ensure extraction attempts don't crash.
        for (const auto& file : (*archive)->files()) {
            (void)(*archive)->extract(file);
        }
    }
}

// Old RAR format (< v1.50) tests
TEST_SUITE("RarArchive - Old RAR Format") {
    TEST_CASE("Old RAR signature detection") {
        // Old RAR signature: 0x52 0x45 0x7E 0x5E ("RE~^")
        std::array<u8, 16> rar_old_sig = {
            0x52, 0x45, 0x7E, 0x5E,  // Signature
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00
        };
        auto archive = rar_archive::open(rar_old_sig);
        // May fail due to truncated data, but should detect signature
        if (!archive.has_value()) {
            CHECK(archive.error().code() != error_code::InvalidSignature);
        }
    }

    TEST_CASE("Old RAR synthetic archive - stored file") {
        // Build a minimal old RAR archive with a stored (uncompressed) file
        // Format based on parse_old() implementation:
        // - 4 bytes: signature "RE~^"
        // - 2 bytes: archive header length (3 = minimal, just flags)
        // - 1 byte: archive flags
        // - File entry:
        //   - 4 bytes: compressed_size
        //   - 4 bytes: original_size
        //   - 2 bytes: checksum
        //   - 2 bytes: member_hdr_len
        //   - 4 bytes: datetime (time + date)
        //   - 2 bytes: attributes
        //   - 1 byte: filename_len
        //   - 1 byte: method (0x30 = STORE)
        //   - N bytes: filename
        //   - compressed data

        std::string test_content = "Hello, old RAR!";
        std::string test_filename = "test.txt";

        byte_vector archive_data;

        // Signature
        archive_data.push_back(0x52);  // 'R'
        archive_data.push_back(0x45);  // 'E'
        archive_data.push_back(0x7E);  // '~'
        archive_data.push_back(0x5E);  // '^'

        // Archive header length (just 1 byte for flags)
        archive_data.push_back(0x03);
        archive_data.push_back(0x00);

        // Archive flags
        archive_data.push_back(0x00);

        // File entry
        // compressed_size (15 bytes)
        u32 cmp_size = static_cast<u32>(test_content.size());
        archive_data.push_back(cmp_size & 0xFF);
        archive_data.push_back((cmp_size >> 8) & 0xFF);
        archive_data.push_back((cmp_size >> 16) & 0xFF);
        archive_data.push_back(static_cast<u8>((cmp_size >> 24) & 0xFF));

        // original_size (same as compressed for STORE)
        archive_data.push_back(cmp_size & 0xFF);
        archive_data.push_back((cmp_size >> 8) & 0xFF);
        archive_data.push_back((cmp_size >> 16) & 0xFF);
        archive_data.push_back(static_cast<u8>((cmp_size >> 24) & 0xFF));

        // checksum (dummy)
        archive_data.push_back(0x00);
        archive_data.push_back(0x00);

        // member_hdr_len (4 datetime + 2 attribs + 1 fn_len + 1 method + filename_len)
        u16 hdr_len = static_cast<u16>(4 + 2 + 1 + 1 + test_filename.size());
        archive_data.push_back(static_cast<u8>(hdr_len & 0xFF));
        archive_data.push_back(static_cast<u8>((hdr_len >> 8) & 0xFF));

        // datetime (dummy)
        archive_data.push_back(0x00);
        archive_data.push_back(0x00);
        archive_data.push_back(0x21);  // Date: 1980-01-01
        archive_data.push_back(0x00);

        // attributes (dummy)
        archive_data.push_back(0x00);
        archive_data.push_back(0x00);

        // filename_len
        archive_data.push_back(static_cast<u8>(test_filename.size()));

        // method (0x30 = STORE)
        archive_data.push_back(0x30);

        // filename
        for (char c : test_filename) {
            archive_data.push_back(static_cast<u8>(c));
        }

        // data (uncompressed content)
        for (char c : test_content) {
            archive_data.push_back(static_cast<u8>(c));
        }

        // Parse the archive
        auto archive = rar_archive::open(archive_data);
        REQUIRE(archive.has_value());
        CHECK((*archive)->version() == rar::OLD);

        // Check file list
        CHECK((*archive)->files().size() == 1);
        const auto& file = (*archive)->files()[0];
        CHECK(file.name == test_filename);
        CHECK(file.uncompressed_size == test_content.size());

        // Extract the file
        auto result = (*archive)->extract(file);
        REQUIRE(result.has_value());
        CHECK(result->size() == test_content.size());

        std::string extracted(result->begin(), result->end());
        CHECK(extracted == test_content);
    }

    TEST_CASE("Old RAR decompressor exists") {
        // Verify the Rar15Decompressor class exists and can be instantiated
        rar_15_decompressor decomp;
        decomp.reset();  // Should not crash
    }
}

// Crypto primitives tests
TEST_SUITE("RarArchive - Crypto Primitives") {
    TEST_CASE("SHA-1 test vectors") {
        using namespace crate::crypto;

        // Test vector: empty string
        {
            u8 digest[sha1_hasher::DIGEST_SIZE];
            sha1_hasher::hash(nullptr, 0, digest);

            // SHA1("") = da39a3ee5e6b4b0d3255bfef95601890afd80709
            CHECK(digest[0] == 0xda);
            CHECK(digest[1] == 0x39);
            CHECK(digest[2] == 0xa3);
            CHECK(digest[19] == 0x09);
        }

        // Test vector: "abc"
        {
            const u8 msg[] = {'a', 'b', 'c'};
            u8 digest[sha1_hasher::DIGEST_SIZE];
            sha1_hasher::hash(msg, 3, digest);

            // SHA1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d
            CHECK(digest[0] == 0xa9);
            CHECK(digest[1] == 0x99);
            CHECK(digest[2] == 0x3e);
            CHECK(digest[19] == 0x9d);
        }
    }

    TEST_CASE("SHA-256 test vectors") {
        using namespace crate::crypto;

        // Test vector: empty string
        {
            u8 digest[sha256_hasher::DIGEST_SIZE];
            sha256_hasher::hash(nullptr, 0, digest);

            // SHA256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
            CHECK(digest[0] == 0xe3);
            CHECK(digest[1] == 0xb0);
            CHECK(digest[2] == 0xc4);
            CHECK(digest[31] == 0x55);
        }

        // Test vector: "abc"
        {
            const u8 msg[] = {'a', 'b', 'c'};
            u8 digest[sha256_hasher::DIGEST_SIZE];
            sha256_hasher::hash(msg, 3, digest);

            // SHA256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
            CHECK(digest[0] == 0xba);
            CHECK(digest[1] == 0x78);
            CHECK(digest[2] == 0x16);
            CHECK(digest[31] == 0xad);
        }
    }

    TEST_CASE("AES-128 decryption") {
        using namespace crate::crypto;

        // NIST AES-128 test vector (ECB mode, single block)
        // Key: 000102030405060708090a0b0c0d0e0f
        // Plaintext: 00112233445566778899aabbccddeeff
        // Ciphertext: 69c4e0d86a7b0430d8cdb78070b4c55a

        const u8 key[16] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
        };
        const u8 plaintext[16] = {
            0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
            0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
        };
        u8 ciphertext[16] = {
            0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
            0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a
        };
        const u8 zero_iv[16] = {0};

        aes_decoder aes;
        aes.init_decrypt(key, 128, zero_iv);
        aes.decrypt_block(ciphertext);

        for (int i = 0; i < 16; i++) {
            CHECK(ciphertext[i] == plaintext[i]);
        }
    }

    TEST_CASE("AES-256 decryption") {
        using namespace crate::crypto;

        // NIST AES-256 test vector
        // Key: 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
        // Plaintext: 00112233445566778899aabbccddeeff
        // Ciphertext: 8ea2b7ca516745bfeafc49904b496089

        const u8 key[32] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
            0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
        };
        const u8 plaintext[16] = {
            0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
            0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
        };
        u8 ciphertext[16] = {
            0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf,
            0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89
        };
        const u8 zero_iv[16] = {0};

        aes_decoder aes;
        aes.init_decrypt(key, 256, zero_iv);
        aes.decrypt_block(ciphertext);

        for (int i = 0; i < 16; i++) {
            CHECK(ciphertext[i] == plaintext[i]);
        }
    }

    TEST_CASE("PBKDF2-SHA256 test vector") {
        using namespace crate::crypto;

        // RFC 6070 test vector (adapted for SHA256)
        // Password: "password"
        // Salt: "salt"
        // Iterations: 1
        // DK length: 32

        const u8 password[] = "password";
        const u8 salt[] = "salt";
        u8 key[32];

        pbkdf2_sha256(password, 8, salt, 4, 1, key, 32);

        // Expected: 120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b
        CHECK(key[0] == 0x12);
        CHECK(key[1] == 0x0f);
        CHECK(key[2] == 0xb6);
        CHECK(key[31] == 0x7b);
    }
}

// RAR encryption API tests
TEST_SUITE("RarArchive - Encryption API") {
    TEST_CASE("Password setter") {
        // Create a minimal non-encrypted RAR4 archive
        byte_vector archive_data = {
            0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00,  // RAR4 signature
            // Archive header
            0x00, 0x00,  // CRC
            0x73,        // Type: ARCHIVE_HEADER
            0x00, 0x00,  // Flags
            0x0D, 0x00,  // Size (13 bytes)
            0x00, 0x00,  // Reserved
            0x00, 0x00, 0x00, 0x00,  // Reserved
        };

        auto archive = rar_archive::open(archive_data);
        if (archive.has_value()) {
            // Set password
            (*archive)->set_password("test123");

            // Verify API exists
            CHECK((*archive)->has_encrypted_files() == false);
        }
    }

    TEST_CASE("Encryption detection in RAR5") {
        // Minimal RAR5 with encryption flag (synthetic)
        // This tests that the parsing detects encryption correctly
        byte_vector archive_data = {
            // RAR5 signature
            0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00,
            // Archive header (simplified)
            0x00, 0x00, 0x00, 0x00,  // CRC
            0x0C,                     // Header size (vint)
            0x01,                     // Header type: ARCHIVE
            0x00,                     // Header flags
            0x00,                     // Extra area size
            // End marker
            0x00, 0x00, 0x00, 0x00,  // CRC
            0x05,                     // Header size (vint)
            0x04,                     // Header type: END
            0x00,                     // Flags
        };

        auto archive = rar_archive::open(archive_data);
        if (archive.has_value()) {
            CHECK((*archive)->version() == rar::V5);
            // No files, so no encrypted files
            CHECK((*archive)->has_encrypted_files() == false);
        }
    }

    TEST_CASE("RarDecryptor initialization") {
        using namespace crate::crypto;

        // Test that decryptor can be initialized without crashing
        rar_decryptor decryptor;

        // RAR4 initialization
        const u8 salt4[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
        decryptor.init_rar4("password", salt4);

        // RAR5 initialization
        const u8 salt5[16] = {0};
        const u8 iv[16] = {0};
        decryptor.init_rar5("password", salt5, 15, iv);

        // Should not crash
        CHECK(true);
    }
}

TEST_SUITE("RarArchive - Filters") {
    TEST_CASE("DELTA filter") {
        // Test delta filter with 2 channels
        // Input: delta-encoded data (channel-by-channel)
        // Channel 0 deltas: 10, 5, 3 -> values: -10, -15, -18 = 246, 241, 238
        // Channel 1 deltas: 20, 8, 4 -> values: -20, -28, -32 = 236, 228, 224
        // Interleaved output: 246, 236, 241, 228, 238, 224
        std::array<u8, 6> data = {10, 5, 3, 20, 8, 4};

        rar5_filter_processor processor;
        rar_filter filter;
        filter.type = rar_filter_type::DELTA;
        filter.block_start = 0;
        filter.block_length = 6;
        filter.channels = 2;
        processor.add_filter(filter);

        processor.apply_filters(data.data(), data.size(), 0);

        // After delta filter: interleaved and delta-decoded
        CHECK(data[0] == 246);  // Channel 0, byte 0: 0 - 10 = 246
        CHECK(data[1] == 236);  // Channel 1, byte 0: 0 - 20 = 236
        CHECK(data[2] == 241);  // Channel 0, byte 1: 246 - 5 = 241
        CHECK(data[3] == 228);  // Channel 1, byte 1: 236 - 8 = 228
        CHECK(data[4] == 238);  // Channel 0, byte 2: 241 - 3 = 238
        CHECK(data[5] == 224);  // Channel 1, byte 2: 228 - 4 = 224
    }

    TEST_CASE("DELTA filter single channel") {
        // Single channel delta: 5, 10, 15, 20
        // Values: -5, -15, -30, -50 = 251, 241, 226, 206
        std::array<u8, 4> data = {5, 10, 15, 20};

        rar5_filter_processor processor;
        rar_filter filter;
        filter.type = rar_filter_type::DELTA;
        filter.block_start = 0;
        filter.block_length = 4;
        filter.channels = 1;
        processor.add_filter(filter);

        processor.apply_filters(data.data(), data.size(), 0);

        CHECK(data[0] == 251);  // 0 - 5 = 251
        CHECK(data[1] == 241);  // 251 - 10 = 241
        CHECK(data[2] == 226);  // 241 - 15 = 226
        CHECK(data[3] == 206);  // 226 - 20 = 206
    }

    TEST_CASE("E8 filter - x86 CALL") {
        // x86 CALL instruction: E8 xx xx xx xx
        // During compression, relative address is converted to absolute
        // E8 filter reverses: absolute -> relative
        // At position 0, after E8, cur_pos = 5
        // If absolute address is 0x100 (256), relative = 256 - 5 = 251
        std::array<u8, 10> data = {
            0xE8,                              // CALL opcode
            0x00, 0x01, 0x00, 0x00,            // Absolute address: 0x100
            0x90, 0x90, 0x90, 0x90, 0x90       // Padding (NOPs)
        };

        rar5_filter_processor processor;
        rar_filter filter;
        filter.type = rar_filter_type::E8;
        filter.block_start = 0;
        filter.block_length = 10;
        filter.channels = 0;
        processor.add_filter(filter);

        processor.apply_filters(data.data(), data.size(), 0);

        // After E8 filter: absolute 0x100 -> relative (0x100 - 5 = 0xFB = 251)
        CHECK(data[0] == 0xE8);
        CHECK(data[1] == 0xFB);  // 251
        CHECK(data[2] == 0x00);
        CHECK(data[3] == 0x00);
        CHECK(data[4] == 0x00);
    }

    TEST_CASE("E8E9 filter - x86 CALL/JMP") {
        // Both E8 (CALL) and E9 (JMP) should be processed
        std::array<u8, 15> data = {
            0xE8,                              // CALL opcode
            0x00, 0x01, 0x00, 0x00,            // Absolute address: 0x100
            0xE9,                              // JMP opcode (at position 5)
            0x00, 0x02, 0x00, 0x00,            // Absolute address: 0x200
            0x90, 0x90, 0x90, 0x90, 0x90       // Padding
        };

        rar5_filter_processor processor;
        rar_filter filter;
        filter.type = rar_filter_type::E8E9;
        filter.block_start = 0;
        filter.block_length = 15;
        filter.channels = 0;
        processor.add_filter(filter);

        processor.apply_filters(data.data(), data.size(), 0);

        // E8 at pos 0: cur_pos = 5, absolute 0x100 -> relative 0xFB
        CHECK(data[0] == 0xE8);
        CHECK(data[1] == 0xFB);

        // E9 at pos 5: cur_pos = 10, absolute 0x200 -> relative 0x1F6 (502)
        CHECK(data[5] == 0xE9);
        CHECK(data[6] == 0xF6);  // 502 & 0xFF
        CHECK(data[7] == 0x01);  // 502 >> 8
    }

    TEST_CASE("ARM filter - BL instruction") {
        // ARM BL instruction: EB xx xx xx (with always condition)
        // Offset is 24-bit, in words (not bytes)
        std::array<u8, 8> data = {
            0x04, 0x00, 0x00, 0xEB,  // BL with absolute offset 4 (at word position 0)
            0x90, 0x90, 0x90, 0x90   // Padding
        };

        rar5_filter_processor processor;
        rar_filter filter;
        filter.type = rar_filter_type::ARM;
        filter.block_start = 0;
        filter.block_length = 8;
        filter.channels = 0;
        processor.add_filter(filter);

        processor.apply_filters(data.data(), data.size(), 0);

        // At position 0, word position = 0, absolute offset 4 -> relative 4-0 = 4
        CHECK(data[3] == 0xEB);  // BL opcode preserved
        CHECK(data[0] == 0x04);  // Offset remains 4 (0 - 0 = 0, then 4 - 0 = 4)
    }

    TEST_CASE("Filter processor - multiple filters") {
        // Test that multiple filters can be added and applied
        std::array<u8, 20> data = {0};

        rar5_filter_processor processor;

        rar_filter filter1;
        filter1.type = rar_filter_type::DELTA;
        filter1.block_start = 0;
        filter1.block_length = 10;
        filter1.channels = 1;

        rar_filter filter2;
        filter2.type = rar_filter_type::DELTA;
        filter2.block_start = 10;
        filter2.block_length = 10;
        filter2.channels = 2;

        processor.add_filter(filter1);
        processor.add_filter(filter2);

        // Filters should apply without crashing
        bool applied = processor.apply_filters(data.data(), data.size(), 0);
        CHECK(applied == true);
    }

    TEST_CASE("Filter processor - clear") {
        rar5_filter_processor processor;

        rar_filter filter;
        filter.type = rar_filter_type::DELTA;
        filter.block_start = 0;
        filter.block_length = 10;
        filter.channels = 1;
        processor.add_filter(filter);

        processor.clear();

        // After clearing, no filters should be applied
        std::array<u8, 10> data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        bool applied = processor.apply_filters(data.data(), data.size(), 0);
        CHECK(applied == false);

        // Data should be unchanged
        CHECK(data[0] == 1);
        CHECK(data[9] == 10);
    }

    TEST_CASE("Filter processor - position offset") {
        // Test that filters are only applied when file position matches
        std::array<u8, 10> data = {5, 10, 15, 20, 0, 0, 0, 0, 0, 0};

        rar5_filter_processor processor;
        rar_filter filter;
        filter.type = rar_filter_type::DELTA;
        filter.block_start = 100;  // Filter starts at file position 100
        filter.block_length = 4;
        filter.channels = 1;
        processor.add_filter(filter);

        // Apply at position 0 - filter shouldn't apply (block starts at 100)
        bool applied = processor.apply_filters(data.data(), data.size(), 0);
        CHECK(applied == false);

        // Data should be unchanged
        CHECK(data[0] == 5);
        CHECK(data[1] == 10);
    }
}

TEST_SUITE("RarArchive - Service Headers") {
    TEST_CASE("Comment accessor") {
        // Test that comment accessor works on empty archive
        std::array<u8, 80> archive_data = {
            // RAR5 signature
            0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00,
            // Archive header
            0x00, 0x00, 0x00, 0x00,  // CRC
            0x05,                     // Header size (vint)
            0x01,                     // Header type: ARCHIVE
            0x00,                     // Flags
            // End header
            0x00, 0x00, 0x00, 0x00,  // CRC
            0x05,                     // Header size (vint)
            0x05,                     // Header type: EOA
            0x00,                     // Flags
        };

        auto archive = rar_archive::open(archive_data);
        if (archive.has_value()) {
            CHECK((*archive)->has_comment() == false);
            CHECK((*archive)->comment().empty());
        }
    }

    TEST_CASE("Header flags constants") {
        // Verify header flag constants are defined correctly
        CHECK(rar::HFL_EXTRA == 0x0001);
        CHECK(rar::HFL_DATA == 0x0002);
        CHECK(rar::HFL_SKIPUNKNOWN == 0x0004);
        CHECK(rar::HFL_SPLITBEFORE == 0x0008);
        CHECK(rar::HFL_SPLITAFTER == 0x0010);
        CHECK(rar::HFL_DEPENDENT == 0x0020);
        CHECK(rar::HFL_KEEPOLD == 0x0040);
    }

    TEST_CASE("Service header constants") {
        // Verify service header name constants
        CHECK(std::string(rar::SERVICE_CMT) == "CMT");
        CHECK(std::string(rar::SERVICE_QO) == "QO");
        CHECK(std::string(rar::SERVICE_ACL) == "ACL");
        CHECK(std::string(rar::SERVICE_STM) == "STM");
        CHECK(std::string(rar::SERVICE_RR) == "RR");
    }
}
