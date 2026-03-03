#pragma once

#include <crate/core/decompressor.hh>
#include <crate/core/bitstream.hh>
#include <crate/core/huffman.hh>
#include <array>
#include <cstring>
#include <memory>

namespace crate {

// LZX mode variants
// Note: Both modes have the E8 translation header at stream start.
// The mode affects reset behavior and possibly other format-specific handling.
enum class lzx_mode {
    cab,  // CAB format
    chm   // CHM format
};

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

class CRATE_EXPORT lzx_decompressor : public bounded_decompressor {
public:
    /// Create an LZX decompressor with validation
    /// @param window_bits Window size in bits (15-21)
    /// @param mode LXZ variant (cab or chm)
    /// @return Decompressor or error if window_bits is invalid
    static result_t<std::unique_ptr<lzx_decompressor>> create(
        unsigned window_bits,
        lzx_mode mode = lzx_mode::cab);

    /// Constructor (prefer using create() for validation)
    /// @param window_bits Window size in bits (15-21, unchecked)
    /// @param mode LXZ variant (cab or chm)
    explicit lzx_decompressor(unsigned window_bits, lzx_mode mode = lzx_mode::cab);

    /// Reset decoder state at a CHM reset interval
    /// Clears decoder state but preserves window contents
    void reset_at_interval();

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    void reset() override;

    // Prepare for a new CAB block (resets bitstream but keeps window/R values)
    void set_expected_output_size(size_t size) override;

private:
    enum class state : u8 {
        READ_CAB_HEADER,       // CAB-specific: read E8 translation flag
        READ_CAB_FILESIZE,     // CAB-specific: read file size for E8 translation
        READ_BLOCK_TYPE,
        READ_BLOCK_SIZE_HI,
        READ_BLOCK_SIZE_LO,
        READ_ALIGNED_TREE,
        READ_MAIN_TREE_0,
        READ_MAIN_TREE_1,
        READ_LENGTH_TREE,
        DECODE_MAIN_SYMBOL,
        READ_LENGTH_SYMBOL,
        READ_OFFSET_VERBATIM,
        READ_OFFSET_ALIGNED,
        COPY_MATCH,
        UNCOMPRESSED_ALIGN,
        UNCOMPRESSED_R0,
        UNCOMPRESSED_R1,
        UNCOMPRESSED_R2,
        UNCOMPRESSED_COPY,
        UNCOMPRESSED_PAD,
        DONE
    };

    enum class tree_state : u8 {
        READ_PRETREE_LENGTHS,
        BUILD_PRETREE,
        DECODE_LENGTHS,
        DONE
    };

    enum class run_state : u8 {
        NONE,
        READ_RUN_BITS,
        READ_REPEAT_SYMBOL,
        FILL_RUN
    };

    void init_state();
    static unsigned calculate_position_slots(unsigned window_bits);

    bool try_peek_bits(const byte*& ptr, const byte* end, unsigned n, u32& out);
    bool try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out);
    bool try_read_byte(const byte*& ptr, const byte* end, u8& out);
    bool try_read_u32_le(const byte*& ptr, const byte* end, u32& out);
    void remove_bits(unsigned n);
    void align_to_byte();

    template<size_t N, unsigned TableBits = HUFFMAN_TABLE_BITS>
    result_t <bool> try_decode(huffman_decoder<N, TableBits>& decoder, u16& out, const byte*& ptr, const byte* end);

    void start_tree_reader(u8* lengths, size_t start, size_t end);
    result_t <bool> advance_tree_reader(const byte*& ptr, const byte* end);

    lzx_mode mode_ = lzx_mode::cab;
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

    state state_ = state::READ_CAB_HEADER;

    u64 bit_buffer_ = 0;
    unsigned bits_left_ = 0;
    size_t total_bits_consumed_ = 0;  // Debug counter

    // CAB-specific header state
    bool cab_header_read_ = false;
    u32 e8_filesize_ = 0;

    u8 block_type_ = 0;
    size_t block_size_ = 0;
    size_t block_remaining_ = 0;
    bool use_aligned_ = false;

    std::array<u8, lzx::NUM_ALIGNED_SYMBOLS> aligned_lengths_{};
    unsigned aligned_len_idx_ = 0;

    tree_state tree_state_ = tree_state::READ_PRETREE_LENGTHS;
    run_state run_state_ = run_state::NONE;
    std::array<u8, 20> pretree_lengths_{};
    unsigned pretree_len_idx_ = 0;
    huffman_decoder<20, 6> pretree_decoder_;  // LZX pretree uses 6-bit tables
    u8* lengths_ptr_ = nullptr;
    size_t lengths_idx_ = 0;
    size_t lengths_end_ = 0;
    u8 run_symbol_ = 0;
    unsigned run_bits_ = 0;
    unsigned run_base_ = 0;
    unsigned run_remaining_ = 0;
    u8 run_value_ = 0;

    u16 main_symbol_ = 0;
    unsigned position_slot_ = 0;
    unsigned length_header_ = 0;
    unsigned match_length_ = 0;
    u32 match_offset_ = 0;
    unsigned match_remaining_ = 0;
    unsigned extra_bits_ = 0;
    unsigned verbatim_bits_needed_ = 0;
    u32 verbatim_bits_ = 0;
    u32 aligned_bits_ = 0;

    u32 uncompressed_value_ = 0;
    unsigned uncompressed_bytes_read_ = 0;

    // Frame boundary tracking (LZX aligns bitstream every 32K output bytes)
    static constexpr u32 LZX_FRAME_SIZE = 32768;
    u32 frame_output_ = 0;  // bytes output since last frame alignment
};

} // namespace crate
