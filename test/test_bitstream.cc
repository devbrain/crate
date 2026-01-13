#include <doctest/doctest.h>
#include <crate/core/bitstream.hh>
#include <array>

using namespace crate;

TEST_SUITE("BitstreamReader") {
    TEST_CASE("LSB-first bit reading") {
        std::array<u8, 4> data = {0b10110100, 0b11001010, 0x00, 0x00};
        lsb_bitstream bs(data);

        // LSB-first: read from right to left within each byte
        // 0b10110100 = bits 0-7: 0,0,1,0,1,1,0,1
        CHECK(bs.read_bit().value() == false);  // bit 0
        CHECK(bs.read_bit().value() == false);  // bit 1
        CHECK(bs.read_bit().value() == true);   // bit 2
        CHECK(bs.read_bit().value() == false);  // bit 3
        CHECK(bs.read_bit().value() == true);   // bit 4
        CHECK(bs.read_bit().value() == true);   // bit 5
        CHECK(bs.read_bit().value() == false);  // bit 6
        CHECK(bs.read_bit().value() == true);   // bit 7
    }

    TEST_CASE("MSB-first bit reading") {
        std::array<u8, 4> data = {0b10110100, 0b11001010, 0x00, 0x00};
        msb_bitstream bs(data);

        // MSB-first: read from left to right within each byte
        // 0b10110100 = bits 7-0: 1,0,1,1,0,1,0,0
        CHECK(bs.read_bit().value() == true);   // bit 7
        CHECK(bs.read_bit().value() == false);  // bit 6
        CHECK(bs.read_bit().value() == true);   // bit 5
        CHECK(bs.read_bit().value() == true);   // bit 4
        CHECK(bs.read_bit().value() == false);  // bit 3
        CHECK(bs.read_bit().value() == true);   // bit 2
        CHECK(bs.read_bit().value() == false);  // bit 1
        CHECK(bs.read_bit().value() == false);  // bit 0
    }

    TEST_CASE("Multi-bit reading LSB") {
        std::array<u8, 4> data = {0xFF, 0x00, 0xAA, 0x55};
        lsb_bitstream bs(data);

        CHECK(bs.read_bits(8).value() == 0xFF);
        CHECK(bs.read_bits(4).value() == 0x00);
        CHECK(bs.read_bits(4).value() == 0x00);
        CHECK(bs.read_bits(8).value() == 0xAA);
    }

    TEST_CASE("Cross-byte boundary reading") {
        std::array<u8, 4> data = {0xF0, 0x0F, 0x00, 0x00};
        lsb_bitstream bs(data);

        // Read 12 bits across byte boundary (LSB first)
        // First byte: 0xF0 = 11110000
        // Second byte: 0x0F = 00001111
        auto first_4 = bs.read_bits(4);
        CHECK(first_4.has_value());
        CHECK(first_4.value() == 0x00);  // Lower 4 bits of 0xF0

        auto next_8 = bs.read_bits(8);
        CHECK(next_8.has_value());
        // Upper 4 bits of 0xF0 (0xF) + lower 4 bits of 0x0F (0xF)
        CHECK(next_8.value() == 0xFF);
    }

    TEST_CASE("Peek and remove bits") {
        std::array<u8, 2> data = {0xAB, 0xCD};
        lsb_bitstream bs(data);

        auto peek1 = bs.peek_bits(4);
        CHECK(peek1.value() == 0x0B);  // Lower 4 bits of 0xAB

        auto peek2 = bs.peek_bits(4);
        CHECK(peek2.value() == 0x0B);  // Same value, not consumed

        bs.remove_bits(4);
        auto peek3 = bs.peek_bits(4);
        CHECK(peek3.value() == 0x0A);  // Upper 4 bits of 0xAB
    }

    TEST_CASE("Byte alignment") {
        std::array<u8, 4> data = {0xFF, 0xAA, 0xBB, 0xCC};
        lsb_bitstream bs(data);

        bs.read_bits(3);  // Read 3 bits
        bs.align_to_byte();  // Should skip remaining 5 bits

        auto byte = bs.read_byte();
        CHECK(byte.value() == 0xAA);
    }

    TEST_CASE("End of stream detection") {
        std::array<u8, 1> data = {0xFF};
        lsb_bitstream bs(data);

        CHECK_FALSE(bs.at_end());
        bs.read_bits(8);
        CHECK(bs.at_end());

        auto result = bs.read_bit();
        CHECK_FALSE(result.has_value());
    }

    TEST_CASE("Read u16 little-endian") {
        std::array<u8, 4> data = {0x34, 0x12, 0x78, 0x56};
        lsb_bitstream bs(data);

        auto val1 = bs.read_u16_le();
        CHECK(val1.has_value());
        CHECK(val1.value() == 0x1234);

        auto val2 = bs.read_u16_le();
        CHECK(val2.has_value());
        CHECK(val2.value() == 0x5678);
    }

    TEST_CASE("Read u32 little-endian") {
        std::array<u8, 4> data = {0x78, 0x56, 0x34, 0x12};
        lsb_bitstream bs(data);

        auto val = bs.read_u32_le();
        CHECK(val.has_value());
        CHECK(val.value() == 0x12345678);
    }
}
