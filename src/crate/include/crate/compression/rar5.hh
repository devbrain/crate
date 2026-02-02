#pragma once

#include <crate/core/decompressor.hh>
#include <crate/compression/rar_common.hh>
#include <crate/compression/rar_filters.hh>
#include <vector>

namespace crate {

// RAR 5.x Decompressor - Streaming implementation
class CRATE_EXPORT rar5_decompressor : public decompressor {
public:
    rar5_decompressor(bool extra_dist = false)
        : extra_dist_(extra_dist) {
        init_state();
    }

    // Enable/disable solid mode (preserves window between files)
    void set_solid_mode(bool solid) { solid_mode_ = solid; }
    bool is_solid_mode() const { return solid_mode_; }

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    [[nodiscard]] bool supports_streaming() const override { return true; }

    void reset() override {
        init_state();
    }

private:
    void init_state();

    // Streaming bit reader
    bool try_peek_bits(const byte*& ptr, const byte* end, unsigned n, u32& out);
    bool try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out);
    void remove_bits(unsigned n);

    void insert_old_dist(size_t distance);

    // Streaming Huffman decode
    bool try_decode_number(const byte*& ptr, const byte* end, const rar_decode_table& dec, unsigned& out);

    // State machine states
    enum class state : u8 {
        // Block header states
        READ_BLOCK_HEADER_ALIGN,
        READ_BLOCK_HEADER_FLAGS,
        READ_BLOCK_HEADER_CHECKSUM,
        READ_BLOCK_HEADER_SIZE,

        // Table reading states
        READ_TABLES_BIT_LENGTH,
        READ_TABLES_BIT_LENGTH_ZERO,
        READ_TABLES_MAIN_SYMBOL,
        READ_TABLES_MAIN_REPEAT,
        READ_TABLES_MAIN_ZEROS,
        BUILD_TABLES,

        // Symbol decoding states
        DECODE_SYMBOL,
        OUTPUT_LITERAL,

        // Filter states
        FILTER_READ_START_COUNT,
        FILTER_READ_START_BYTES,
        FILTER_READ_LENGTH_COUNT,
        FILTER_READ_LENGTH_BYTES,
        FILTER_READ_TYPE,
        FILTER_READ_CHANNELS,

        // Repeat/match states
        REPEAT_LAST,
        SHORT_REPEAT_DECODE_LENGTH,
        SHORT_REPEAT_LENGTH_EXTRA,
        MATCH_LENGTH_EXTRA,
        MATCH_DECODE_DIST,
        MATCH_DIST_EXTRA_HIGH,
        MATCH_DIST_DECODE_LOW,
        MATCH_DIST_EXTRA_LOW,

        COPY_MATCH,
        CHECK_BLOCK_END,
        DONE
    };

    struct BlockHeader {
        int block_size = 0;
        int block_bit_size = 0;
        bool last_block = false;
        bool table_present = false;
    };

    // Bit buffer for streaming
    u64 bit_buffer_ = 0;
    unsigned bits_left_ = 0;
    size_t total_bits_consumed_ = 0;  // Total bits consumed from input stream

    // State machine
    state state_ = state::READ_BLOCK_HEADER_ALIGN;

    // Configuration
    bool extra_dist_ = false;
    bool solid_mode_ = false;

    // Window buffer
    std::vector<u8> window_;
    size_t max_win_size_ = 0;
    size_t max_win_mask_ = 0;
    size_t unp_ptr_ = 0;
    size_t wr_ptr_ = 0;
    bool first_win_done_ = false;
    bool tables_read_ = false;

    // Distance history
    std::array<size_t, 4> old_dist_{};
    unsigned last_length_ = 0;

    // Block state
    BlockHeader block_header_;
    size_t block_start_bit_offset_ = 0;  // Bit offset in stream at block content start

    // Huffman tables
    rar_block_tables tables_;

    // Table reading state
    std::array<u8, rar::BC> bit_lengths_{};
    std::vector<u8> main_table_;
    unsigned table_index_ = 0;
    unsigned table_size_ = 0;
    unsigned cur_length_ = 0;
    unsigned repeat_count_ = 0;

    // Block header reading state
    u8 header_flags_ = 0;
    u8 header_checksum_ = 0;
    unsigned header_byte_count_ = 0;
    unsigned header_bytes_read_ = 0;

    // Symbol decoding state
    unsigned cur_symbol_ = 0;
    unsigned cur_dist_slot_ = 0;
    unsigned cur_d_bits_ = 0;
    size_t cur_distance_ = 0;
    unsigned match_remaining_ = 0;
    unsigned dist_num_ = 0;

    // Filter state
    u64 filter_block_start_ = 0;
    u64 filter_block_length_ = 0;
    unsigned filter_type_ = 0;
    unsigned vint_byte_count_ = 0;
    unsigned vint_bytes_read_ = 0;
    u64 vint_value_ = 0;

    // Length slot decoding state
    unsigned length_slot_ = 0;
    unsigned length_lbits_ = 0;

    // Filter processor
    rar5_filter_processor filter_processor_;
    u64 written_file_pos_ = 0;

    bool initialized_ = false;
};

}  // namespace crate
