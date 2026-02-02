#pragma once

#include <crate/core/decompressor.hh>
#include <crate/compression/rar_common.hh>
#include <crate/compression/rar_ppm.hh>
#include <memory>
#include <vector>

namespace crate {

// RAR 2.9/3.x Decompressor - Streaming implementation (LZ77 + PPMd support)
// Note: PPM mode still requires complete input; LZ mode is fully streaming
class CRATE_EXPORT rar_29_decompressor : public decompressor {
public:
    rar_29_decompressor() {
        init_tables();
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
        solid_mode_ = false;
    }

private:
    void init_state();
    void init_tables();

    // Streaming bit reader
    bool try_peek_bits(const byte*& ptr, const byte* end, unsigned n, u32& out);
    bool try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out);
    void remove_bits(unsigned n);

    // Streaming Huffman decode
    bool try_decode_number(const byte*& ptr, const byte* end, const rar_decode_table& dec, unsigned& out);

    void insert_old_dist(size_t distance);

    // State machine states
    enum class state : u8 {
        // Initial state
        READ_TABLES_ALIGN,
        READ_TABLES_CHECK_PPM,

        // PPM mode (non-streaming fallback)
        PPM_MODE,

        // Table reading states (LZ mode)
        READ_TABLES_CHECK_OLD,
        READ_TABLES_BIT_LENGTH,
        READ_TABLES_BIT_LENGTH_ZERO,
        READ_TABLES_MAIN_SYMBOL,
        READ_TABLES_MAIN_REPEAT,
        READ_TABLES_MAIN_ZEROS,
        BUILD_TABLES,

        // Symbol decoding states
        DECODE_SYMBOL,
        OUTPUT_LITERAL,

        // Match states (number >= 271)
        MATCH_READ_LENGTH_EXTRA,
        MATCH_DECODE_DIST,
        MATCH_READ_DIST_EXTRA,
        MATCH_READ_DIST_EXTRA_HIGH,
        MATCH_READ_LOW_DIST,

        // End of block states
        END_OF_BLOCK_CHECK,

        // Repeat states (258, 259-262)
        REPEAT_LAST,
        REPEAT_OLD_DIST_DECODE_LENGTH,
        REPEAT_OLD_DIST_LENGTH_EXTRA,

        // Short distance states (263-270)
        SHORT_DIST_READ_EXTRA,

        COPY_MATCH,
        DONE
    };

    // Static decode tables for short distance codes
    static constexpr u8 sd_decode_[8] = {0, 4, 8, 16, 32, 64, 128, 192};
    static constexpr u8 sd_bits_[8] = {2, 2, 3, 4, 5, 6, 6, 6};

    // Bit buffer for streaming
    u64 bit_buffer_ = 0;
    unsigned bits_left_ = 0;
    size_t total_bits_consumed_ = 0;

    // State machine
    state state_ = state::READ_TABLES_ALIGN;

    // Configuration
    bool solid_mode_ = false;

    // Window buffer
    std::vector<u8> window_;
    size_t unp_ptr_ = 0;
    size_t wr_ptr_ = 0;
    bool first_win_done_ = false;
    bool tables_read_ = false;

    // Distance history
    std::array<size_t, 4> old_dist_{};
    size_t old_dist_ptr_ = 0;
    unsigned last_length_ = 0;
    unsigned low_dist_rep_count_ = 0;
    unsigned prev_low_dist_ = 0;

    // Huffman tables
    rar_block_tables_30 tables_;
    std::array<u8, rar::HUFF_TABLE_SIZE30> old_table_{};

    // Static decode tables (length/distance)
    std::array<u8, 28> ldecode_{};
    std::array<u8, 28> lbits_{};
    std::array<int, rar::DC30> ddecode_{};
    std::array<u8, rar::DC30> dbits_{};

    // Table reading state
    std::array<u8, rar::BC30> bit_lengths_{};
    std::array<u8, rar::HUFF_TABLE_SIZE30> main_table_{};
    unsigned table_index_ = 0;
    unsigned cur_length_ = 0;
    unsigned repeat_count_ = 0;

    // Current operation state
    unsigned cur_symbol_ = 0;
    unsigned cur_length_slot_ = 0;
    unsigned cur_length_bits_ = 0;
    unsigned cur_dist_slot_ = 0;
    unsigned cur_dist_bits_ = 0;
    size_t cur_distance_ = 0;
    unsigned match_remaining_ = 0;
    unsigned dist_num_ = 0;

    // PPM support
    bool ppm_mode_ = false;
    int ppm_esc_char_ = 2;
    rar::ppm::model_ppm ppm_model_;
    std::unique_ptr<rar::ppm::input_adapter> ppm_input_;
    byte_span input_span_;

    bool initialized_ = false;
};

}  // namespace crate
