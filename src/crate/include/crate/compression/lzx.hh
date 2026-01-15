#pragma once

#include <crate/core/decompressor.hh>
#include <crate/core/bitstream.hh>
#include <crate/core/huffman.hh>
#include <array>
#include <cstring>
#include <memory>

namespace crate {

// LZX constants
namespace lzx {
    // Valid window size range (in bits)
    constexpr unsigned MIN_WINDOW_BITS = 15;  // 32KB minimum
    constexpr unsigned MAX_WINDOW_BITS = 21;  // 2MB maximum

    constexpr unsigned MIN_MATCH = 2;
    constexpr unsigned MAX_MATCH = 257;
    constexpr unsigned NUM_CHARS = 256;
    constexpr unsigned NUM_PRIMARY_LENGTHS = 7;
    constexpr unsigned NUM_SECONDARY_LENGTHS = 249;

    constexpr unsigned BLOCKTYPE_INVALID = 0;
    constexpr unsigned BLOCKTYPE_VERBATIM = 1;
    constexpr unsigned BLOCKTYPE_ALIGNED = 2;
    constexpr unsigned BLOCKTYPE_UNCOMPRESSED = 3;

    constexpr unsigned NUM_ALIGNED_SYMBOLS = 8;

    // Position slot base values and extra bits
    constexpr std::array<u32, 51> position_base = {
        0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192,
        256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144,
        8192, 12288, 16384, 24576, 32768, 49152, 65536, 98304,
        131072, 196608, 262144, 393216, 524288, 655360, 786432,
        917504, 1048576, 1179648, 1310720, 1441792, 1572864,
        1703936, 1835008, 1966080, 2097152
    };

    constexpr std::array<u8, 51> extra_bits = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
        14, 14, 15, 15, 16, 16, 17, 17, 17, 17, 17, 17,
        17, 17, 17, 17, 17, 17, 17, 17, 17
    };
}

class CRATE_EXPORT lzx_decompressor : public decompressor {
public:
    /// Create an LZX decompressor with validation
    /// @param window_bits Window size in bits (15-21)
    /// @return Decompressor or error if window_bits is invalid
    static result_t<std::unique_ptr<lzx_decompressor>> create(unsigned window_bits);

    /// Constructor (prefer using create() for validation)
    /// @param window_bits Window size in bits (15-21, unchecked)
    explicit lzx_decompressor(unsigned window_bits);

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    void reset() override;

private:
    void init_state();
    static unsigned calculate_position_slots(unsigned window_bits);

    void_result_t read_aligned_tree(msb_bitstream& bs);

    void_result_t read_main_and_length_trees(msb_bitstream& bs);

    static void_result_t read_lengths_with_pretree(msb_bitstream& bs,
                                          std::span<u8> lengths,
                                          size_t start, size_t end);

    void_result_t decompress_block(msb_bitstream& bs, mutable_byte_span output,
                                 size_t& out_pos, size_t block_size, bool use_aligned);

    unsigned window_bits_ = 0;
    u32 window_size_ = 0;
    byte_vector window_;
    u32 window_pos_ = 0;
    unsigned num_position_slots_ = 0;

    u32 R0_ = 1, R1_ = 1, R2_ = 1;

    std::array<u8, 720> main_lengths_{};
    std::array<u8, lzx::NUM_SECONDARY_LENGTHS> length_lengths_{};

    lzx_main_decoder main_decoder_;
    lzx_length_decoder length_decoder_;
    lzx_aligned_decoder aligned_decoder_;
};

} // namespace crate
