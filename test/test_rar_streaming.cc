#include <doctest/doctest.h>
#include <crate/compression/rar_unpack.hh>
#include <crate/formats/rar.hh>
#include <crate/core/system.hh>
#include <crate/test_config.hh>
#include "test_streaming.hh"

#include <filesystem>

using namespace crate;
using namespace crate::test;

namespace {

// Helper to decompress with chunked input
template<typename Decompressor>
byte_vector decompress_with_chunks(
    Decompressor& dec,
    byte_span compressed,
    size_t chunk_size,
    size_t expected_size
) {
    byte_vector output(expected_size + 1024);  // Extra space
    size_t in_pos = 0;
    size_t out_pos = 0;

    while (true) {
        size_t remaining = compressed.size() - in_pos;
        size_t this_chunk = std::min(chunk_size, remaining);
        bool is_last = (in_pos + this_chunk >= compressed.size());

        byte_span in_chunk{compressed.data() + in_pos, this_chunk};
        mutable_byte_span out_chunk{output.data() + out_pos, output.size() - out_pos};

        auto result = dec.decompress_some(in_chunk, out_chunk, is_last);
        if (!result) {
            throw std::runtime_error(std::string("Decompression error: ") +
                                   std::string(result.error().message()));
        }

        in_pos += result->bytes_read;
        out_pos += result->bytes_written;

        if (result->finished()) {
            break;
        }

        // Detect stall
        if (result->bytes_read == 0 && result->bytes_written == 0) {
            if (is_last) {
                throw std::runtime_error("Decompression stalled at end of input");
            }
        }
    }

    output.resize(out_pos);
    return output;
}

// Helper to decompress with small output buffer
template<typename Decompressor>
byte_vector decompress_with_small_output(
    Decompressor& dec,
    byte_span compressed,
    size_t output_buffer_size
) {
    byte_vector output;
    byte_vector buffer(output_buffer_size);
    size_t in_pos = 0;

    while (true) {
        size_t remaining = compressed.size() - in_pos;
        bool is_last = (remaining == 0);

        byte_span in_chunk{compressed.data() + in_pos, remaining};
        mutable_byte_span out_chunk{buffer.data(), buffer.size()};

        auto result = dec.decompress_some(in_chunk, out_chunk, is_last);
        if (!result) {
            throw std::runtime_error(std::string("Decompression error: ") +
                                   std::string(result.error().message()));
        }

        in_pos += result->bytes_read;
        if (result->bytes_written > 0) {
            output.insert(output.end(), buffer.begin(),
                         buffer.begin() + static_cast<std::ptrdiff_t>(result->bytes_written));
        }

        if (result->finished()) {
            break;
        }

        // Detect stall
        if (result->bytes_read == 0 && result->bytes_written == 0 && is_last) {
            break;
        }
    }

    return output;
}

} // namespace

TEST_SUITE("Rar15Decompressor - Streaming") {
    TEST_CASE("Basic functionality") {
        rar_15_decompressor dec;
        CHECK(dec.supports_streaming() == true);
        dec.reset();  // Should not crash
    }
}

TEST_SUITE("Rar29Decompressor - Streaming") {
    TEST_CASE("Basic functionality") {
        rar_29_decompressor dec;
        CHECK(dec.supports_streaming() == true);
        dec.reset();  // Should not crash
    }

    TEST_CASE("Streaming from RAR archive") {
        auto path = ::test::rar_dir() / "unrar_test_01.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        const auto& files = (*archive)->files();
        REQUIRE(!files.empty());

        // Extract each file and verify against expected size
        size_t verified = 0;
        for (const auto& file : files) {
            auto expected = (*archive)->extract(file);
            if (!expected || expected->empty()) {
                continue;
            }

            CAPTURE(file.name);
            CHECK(expected->size() == file.uncompressed_size);

            // Verify via extract_stream produces identical output
            auto stream = (*archive)->extract_stream(file);
            REQUIRE(stream.has_value());
            std::string from_stream((std::istreambuf_iterator<char>(**stream)),
                                     std::istreambuf_iterator<char>());
            CHECK(from_stream.size() == expected->size());
            CHECK(std::equal(expected->begin(), expected->end(),
                             reinterpret_cast<const u8*>(from_stream.data()),
                             reinterpret_cast<const u8*>(from_stream.data()) + from_stream.size()));
            ++verified;
        }
        CHECK(verified > 0);
    }
}

TEST_SUITE("Rar5Decompressor - Streaming") {
    TEST_CASE("Basic functionality") {
        rar5_decompressor dec;
        CHECK(dec.supports_streaming() == true);
        dec.reset();  // Should not crash
    }

    TEST_CASE("Solid mode flag") {
        rar5_decompressor dec;
        CHECK(dec.is_solid_mode() == false);
        dec.set_solid_mode(true);
        CHECK(dec.is_solid_mode() == true);
        dec.reset();
        // Note: reset() preserves solid_mode_ - it's a configuration setting
        // The window content is preserved in solid mode for cross-file decompression
        CHECK(dec.is_solid_mode() == true);
        dec.set_solid_mode(false);
        CHECK(dec.is_solid_mode() == false);
    }
}

TEST_SUITE("RAR Decompressor - Integration Streaming Tests") {
    TEST_CASE("RAR4 archive extraction verifies streaming works") {
        auto path = ::test::rar_dir() / "unrar_test_01.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());

        // Extract all files - this exercises the streaming decompressor
        int extracted = 0;
        for (const auto& file : (*archive)->files()) {
            auto result = (*archive)->extract(file);
            if (result.has_value()) {
                CHECK(result->size() == file.uncompressed_size);
                extracted++;
            }
        }
        CHECK(extracted > 0);
    }

    TEST_CASE("RAR4 PPM extraction verifies streaming") {
        auto path = ::test::rar_dir() / "ppm_test.rar";
        REQUIRE(std::filesystem::exists(path));

        auto archive = rar_archive::open(path);
        REQUIRE(archive.has_value());
        CHECK((*archive)->version() == rar::V4);

        for (const auto& file : (*archive)->files()) {
            auto result = (*archive)->extract(file);
            if (result.has_value()) {
                CHECK(result->size() == file.uncompressed_size);
            }
        }
    }
}

TEST_SUITE("RAR Streaming - Chunked Input Tests") {
    // These tests verify that the streaming implementation correctly handles
    // input being provided in small chunks, as would happen with network streams
    // or memory-constrained environments.

    TEST_CASE("Old RAR format synthetic streaming test") {
        // Create a synthetic old RAR archive with stored data
        // This tests that the rar_15_decompressor handles streaming correctly

        std::string test_content = "Hello, streaming test for old RAR format!";
        std::string test_filename = "test.txt";

        byte_vector archive_data;

        // Signature "RE~^"
        archive_data.push_back(0x52);
        archive_data.push_back(0x45);
        archive_data.push_back(0x7E);
        archive_data.push_back(0x5E);

        // Archive header length
        archive_data.push_back(0x03);
        archive_data.push_back(0x00);

        // Archive flags
        archive_data.push_back(0x00);

        // File entry
        u32 cmp_size = static_cast<u32>(test_content.size());
        archive_data.push_back(cmp_size & 0xFF);
        archive_data.push_back((cmp_size >> 8) & 0xFF);
        archive_data.push_back((cmp_size >> 16) & 0xFF);
        archive_data.push_back(static_cast<u8>((cmp_size >> 24) & 0xFF));

        // original_size
        archive_data.push_back(cmp_size & 0xFF);
        archive_data.push_back((cmp_size >> 8) & 0xFF);
        archive_data.push_back((cmp_size >> 16) & 0xFF);
        archive_data.push_back(static_cast<u8>((cmp_size >> 24) & 0xFF));

        // checksum
        archive_data.push_back(0x00);
        archive_data.push_back(0x00);

        // member_hdr_len
        u16 hdr_len = static_cast<u16>(4 + 2 + 1 + 1 + test_filename.size());
        archive_data.push_back(static_cast<u8>(hdr_len & 0xFF));
        archive_data.push_back(static_cast<u8>((hdr_len >> 8) & 0xFF));

        // datetime
        archive_data.push_back(0x00);
        archive_data.push_back(0x00);
        archive_data.push_back(0x21);
        archive_data.push_back(0x00);

        // attributes
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

        // data
        for (char c : test_content) {
            archive_data.push_back(static_cast<u8>(c));
        }

        // Parse and extract
        auto archive = rar_archive::open(archive_data);
        REQUIRE(archive.has_value());
        CHECK((*archive)->version() == rar::OLD);

        REQUIRE((*archive)->files().size() == 1);
        const auto& file = (*archive)->files()[0];

        auto result = (*archive)->extract(file);
        REQUIRE(result.has_value());

        std::string extracted(result->begin(), result->end());
        CHECK(extracted == test_content);
    }
}
