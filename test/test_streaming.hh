#pragma once

#include <crate/core/decompressor.hh>
#include <doctest/doctest.h>
#include <vector>
#include <random>
#include <functional>
#include <string>

namespace crate::test {

// Test configuration for streaming decompressor tests
struct streaming_test_config {
    std::string name;                           // Test name for reporting
    std::vector<u8> compressed;                 // Compressed input data
    std::vector<u8> expected;                   // Expected decompressed output
    std::vector<size_t> chunk_sizes = {1, 2, 3, 5, 7, 11, 13, 17, 64, 256};
    std::vector<size_t> output_buffer_sizes = {};  // Empty = use expected.size()
    bool test_random_chunks = true;
    int random_trials = 10;
    unsigned random_seed = 42;
};

// Result of a streaming test
struct streaming_test_result {
    bool success = true;
    std::string error_message;
    size_t chunk_size = 0;
    size_t output_buffer_size = 0;
};

// Factory function type for creating decompressors
using decompressor_factory = std::function<std::unique_ptr<decompressor>()>;

// Decompress using streaming with specified chunk sizes
inline byte_vector decompress_chunked(
    decompressor& dec,
    byte_span compressed,
    size_t input_chunk_size,
    size_t output_buffer_size
) {
    byte_vector output(output_buffer_size);
    size_t input_pos = 0;
    size_t output_pos = 0;

    while (true) {
        size_t remaining_input = compressed.size() - input_pos;
        size_t this_chunk = std::min(input_chunk_size, remaining_input);
        bool is_last = (input_pos + this_chunk >= compressed.size());

        byte_span in_chunk{compressed.data() + input_pos, this_chunk};
        
        // Provide remaining output space
        size_t out_space = output.size() - output_pos;
        if (out_space == 0) {
            // Grow output buffer
            output.resize(output.size() + output_buffer_size);
            out_space = output.size() - output_pos;
        }
        mutable_byte_span out_chunk{output.data() + output_pos, out_space};

        auto result = dec.decompress_some(in_chunk, out_chunk, is_last);
        if (!result) {
            throw std::runtime_error(std::string("Decompression error: ") + std::string(result.error().message()));
        }

        input_pos += result->bytes_read;
        output_pos += result->bytes_written;

        if (result->finished()) {
            break;
        }

        // Detect stall (no progress)
        if (result->bytes_read == 0 && result->bytes_written == 0 && !is_last) {
            // Need more input but none available - this is OK, continue
            if (this_chunk == 0) {
                break;  // No input and no progress = done
            }
        }
    }

    output.resize(output_pos);
    return output;
}

// Run systematic streaming tests on a decompressor
inline void run_streaming_tests(
    const streaming_test_config& config,
    decompressor_factory factory
) {
    INFO("Test: " << config.name);
    auto prepare_decompressor = [&](decompressor& dec) {
        if (dec.requires_output_size()) {
            dec.set_expected_output_size(config.expected.size());
        }
    };

    // First verify one-shot decompression works
    {
        auto dec = factory();
        size_t out_size = config.expected.size();
        if (!dec->requires_output_size()) {
            out_size += 1024;  // Extra space for unbounded codecs
        }
        byte_vector output(out_size);
        auto result = dec->decompress(config.compressed, output);
        REQUIRE_MESSAGE(result.has_value(), "One-shot decompression failed");
        output.resize(*result);
        REQUIRE_MESSAGE(output == config.expected, 
            "One-shot result doesn't match expected (size: " << output.size() 
            << " vs " << config.expected.size() << ")");
    }

    // Test with various input chunk sizes
    for (size_t chunk_size : config.chunk_sizes) {
        if (chunk_size > config.compressed.size() + 1) continue;

        CAPTURE(chunk_size);
        auto dec = factory();
        prepare_decompressor(*dec);
        
        size_t out_size = config.expected.size() + 1024;
        auto result = decompress_chunked(*dec, config.compressed, chunk_size, out_size);
        
        CHECK_MESSAGE(result.size() == config.expected.size(),
            "Chunk size " << chunk_size << ": output size mismatch - got " 
            << result.size() << ", expected " << config.expected.size());
        CHECK_MESSAGE(result == config.expected,
            "Chunk size " << chunk_size << ": output content mismatch");
    }

    // Test with various output buffer sizes (if specified)
    if (!config.output_buffer_sizes.empty()) {
        for (size_t out_buf_size : config.output_buffer_sizes) {
            CAPTURE(out_buf_size);
            auto dec = factory();
            prepare_decompressor(*dec);
            
            auto result = decompress_chunked(*dec, config.compressed, 
                                             config.compressed.size(), out_buf_size);
            
            CHECK_MESSAGE(result.size() == config.expected.size(),
                "Output buffer " << out_buf_size << ": size mismatch");
            CHECK_MESSAGE(result == config.expected,
                "Output buffer " << out_buf_size << ": content mismatch");
        }
    }

    // Test with random chunk sizes
    if (config.test_random_chunks) {
        std::mt19937 rng(config.random_seed);
        size_t max_chunk = std::max(size_t(1), config.compressed.size() / 2);
        std::uniform_int_distribution<size_t> dist(1, std::max(size_t(2), max_chunk));

        for (int trial = 0; trial < config.random_trials; trial++) {
            CAPTURE(trial);
            auto dec = factory();
            prepare_decompressor(*dec);
            byte_vector output(config.expected.size() + 1024);
            size_t in_pos = 0;
            size_t out_pos = 0;

            while (in_pos < config.compressed.size()) {
                size_t chunk = std::min(dist(rng), config.compressed.size() - in_pos);
                bool is_last = (in_pos + chunk >= config.compressed.size());

                byte_span in_chunk{config.compressed.data() + in_pos, chunk};
                mutable_byte_span out_chunk{output.data() + out_pos, 
                                            output.size() - out_pos};

                auto result = dec->decompress_some(in_chunk, out_chunk, is_last);
                REQUIRE(result.has_value());

                in_pos += result->bytes_read;
                out_pos += result->bytes_written;

                if (result->finished()) break;
            }

            output.resize(out_pos);
            CHECK_MESSAGE(output == config.expected,
                "Random trial " << trial << ": content mismatch");
        }
    }
}

// Generate test data: all literals (control byte 0xFF)
inline std::pair<std::vector<u8>, std::vector<u8>> 
generate_lzss_literals(const std::string& text) {
    std::vector<u8> compressed;
    std::vector<u8> expected(text.begin(), text.end());

    size_t pos = 0;
    while (pos < text.size()) {
        compressed.push_back(0xFF);  // All 8 bits are literals
        for (int i = 0; i < 8 && pos < text.size(); i++) {
            compressed.push_back(static_cast<u8>(text[pos++]));
        }
    }

    return {compressed, expected};
}

// Generate test data: pattern with matches
// Creates: N literal blocks (8 literals each), then M match blocks (8 matches each)
inline std::pair<std::vector<u8>, std::vector<u8>>
generate_lzss_with_matches(size_t literal_blocks, size_t match_blocks) {
    std::vector<u8> compressed;
    std::vector<u8> expected;

    // Initial position in window (SZDD starts at 4096-16 = 4080)
    constexpr u32 INITIAL_POS = 4096 - 16;

    // First, write literal blocks (8 literals per block)
    for (size_t block = 0; block < literal_blocks; block++) {
        compressed.push_back(0xFF);  // All 8 bits are literals
        for (int i = 0; i < 8; i++) {
            u8 val = static_cast<u8>('A' + (expected.size() % 26));
            compressed.push_back(val);
            expected.push_back(val);
        }
    }

    // Now add match blocks that copy from the literals
    // Each match block has 8 matches, each copying 3 bytes (minimum match length)
    if (match_blocks > 0 && literal_blocks > 0) {
        u16 match_src = INITIAL_POS;  // Copy from start of our literals
        constexpr u8 match_len = 3;   // Minimum match length

        for (size_t block = 0; block < match_blocks; block++) {
            compressed.push_back(0x00);  // All 8 bits are matches

            for (int i = 0; i < 8; i++) {
                // Match encoding: lo = pos & 0xFF, hi = ((pos >> 4) & 0xF0) | (len - 3)
                u8 lo = static_cast<u8>(match_src & 0xFF);
                u8 hi = static_cast<u8>(((match_src >> 4) & 0xF0) | ((match_len - 3) & 0x0F));
                compressed.push_back(lo);
                compressed.push_back(hi);

                // Add expected output - copy first 3 bytes of our literals
                for (u8 j = 0; j < match_len; j++) {
                    expected.push_back(expected[j % (literal_blocks * 8)]);
                }
            }
        }
    }

    return {compressed, expected};
}

struct lsb_bit_writer {
    std::vector <u8> data;
    u8 current = 0;
    unsigned bit_pos = 0;

    void push_bit(unsigned bit) {
        if (bit) {
            current |= static_cast <u8>(1u << bit_pos);
        }
        bit_pos++;
        if (bit_pos == 8) {
            data.push_back(current);
            current = 0;
            bit_pos = 0;
        }
    }

    void push_bits(u32 value, unsigned count) {
        for (unsigned i = 0; i < count; i++) {
            push_bit((value >> i) & 1u);
        }
    }

    std::vector <u8> finish() {
        if (bit_pos > 0) {
            data.push_back(current);
        }
        return data;
    }
};

inline std::vector <u8> make_kwaj_lzss_literals(const std::vector <u8>& literals) {
    lsb_bit_writer writer;
    for (u8 value : literals) {
        writer.push_bit(1);
        writer.push_bits(value, 8);
    }
    return writer.finish();
}

} // namespace crate::test
