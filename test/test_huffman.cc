#include <doctest/doctest.h>
#include <crate/core/huffman.hh>
#include <array>

using namespace crate;

TEST_SUITE("HuffmanDecoder") {
    TEST_CASE("Build empty table") {
        huffman_decoder<8> decoder;
        std::array<u8, 8> lengths = {0, 0, 0, 0, 0, 0, 0, 0};

        auto result = decoder.build(lengths);
        CHECK(result.has_value());
    }

    TEST_CASE("Build fixed Huffman table") {
        huffman_decoder<288> decoder;

        // Build fixed literal/length table (RFC 1951)
        std::array<u8, 288> lengths{};
        std::fill(lengths.begin(), lengths.begin() + 144, 8);
        std::fill(lengths.begin() + 144, lengths.begin() + 256, 9);
        std::fill(lengths.begin() + 256, lengths.begin() + 280, 7);
        std::fill(lengths.begin() + 280, lengths.end(), 8);

        auto result = decoder.build(lengths);
        CHECK(result.has_value());
    }

    TEST_CASE("Build simple 2-symbol table") {
        huffman_decoder<2> decoder;
        std::array<u8, 2> lengths = {1, 1};  // A=0, B=1

        auto result = decoder.build(lengths);
        CHECK(result.has_value());
    }

    TEST_CASE("Decode simple Huffman codes LSB") {
        // Simple 2-symbol Huffman code: A=0, B=1
        huffman_decoder<2> decoder;
        std::array<u8, 2> lengths = {1, 1};

        auto build_result = decoder.build(lengths);
        REQUIRE(build_result.has_value());

        // Bit stream: 10101010 = alternating B,A,B,A... (LSB first)
        std::array<u8, 2> data = {0b10101010, 0x00};
        lsb_bitstream bs(data);

        // LSB first: first bit read is bit 0 (rightmost)
        auto sym1 = decoder.decode(bs);
        REQUIRE(sym1.has_value());
        CHECK(sym1.value() == 0);  // bit 0 = 0 -> A

        auto sym2 = decoder.decode(bs);
        REQUIRE(sym2.has_value());
        CHECK(sym2.value() == 1);  // bit 1 = 1 -> B

        auto sym3 = decoder.decode(bs);
        REQUIRE(sym3.has_value());
        CHECK(sym3.value() == 0);  // bit 2 = 0 -> A

        auto sym4 = decoder.decode(bs);
        REQUIRE(sym4.has_value());
        CHECK(sym4.value() == 1);  // bit 3 = 1 -> B
    }

    TEST_CASE("Decode unbalanced Huffman tree") {
        // 3-symbol code: A=0 (len 1), B=10 (len 2), C=11 (len 2)
        huffman_decoder<3> decoder;
        std::array<u8, 3> lengths = {1, 2, 2};

        auto build_result = decoder.build(lengths);
        REQUIRE(build_result.has_value());

        // Test decoding symbol A (code 0)
        std::array<u8, 1> data_a = {0b00000000};
        lsb_bitstream bs_a(data_a);
        auto sym_a = decoder.decode(bs_a);
        CHECK(sym_a.has_value());
        CHECK(sym_a.value() == 0);  // A
    }

    TEST_CASE("Invalid code length") {
        huffman_decoder<4> decoder;
        std::array<u8, 4> lengths = {20, 1, 1, 1};  // 20 > MAX_CODE_LENGTH

        auto result = decoder.build(lengths);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code() == error_code::InvalidHuffmanTable);
    }

    TEST_CASE("Too many symbols") {
        huffman_decoder<4> decoder;
        std::array<u8, 8> lengths = {1, 1, 2, 2, 3, 3, 3, 3};

        auto result = decoder.build(lengths);  // 8 > MaxSymbols(4)
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code() == error_code::InvalidHuffmanTable);
    }
}
