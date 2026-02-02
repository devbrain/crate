#pragma once

#include <crate/core/decompressor.hh>
#include <crate/compression/rar_common.hh>
#include <vector>

namespace crate {

// RAR 1.5 Decompressor (Old RAR format, < v1.50)
// Uses adaptive Huffman coding with 64KB sliding window
// Based on unrar unpack15.cc algorithm
// Streaming implementation with resumable state machine
class CRATE_EXPORT rar_15_decompressor : public decompressor {
public:
    static constexpr size_t WINDOW_SIZE = 0x10000; // 64KB window
    static constexpr size_t WINDOW_MASK = WINDOW_SIZE - 1;

    rar_15_decompressor() {
        init_state();
    }

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
    void init_huff();
    static void corr_huff(u16* ch_set, unsigned* ntopl);

    // Streaming bit reader
    bool try_peek_bits(const byte*& ptr, const byte* end, unsigned n, u32& out);
    bool try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out);
    void remove_bits(unsigned n);

    void insert_old_dist(unsigned distance);

    // State machine states
    enum class state : u8 {
        READ_FLAG_BYTE,        // Read 8 flag bits
        CHECK_FLAG1,           // Check first flag bit (0=literal, 1=LZ)
        CHECK_FLAG2,           // Check second flag bit (0=short, 1=long)

        // Huffman literal decode
        HUFF_DECODE,           // Decode literal byte
        HUFF_READ_EXTRA,       // Read extra bits for position

        // Short LZ states
        SHORT_LZ_CHECK_REPEAT, // Check if short LZ is a repeat
        SHORT_LZ_READ_LENGTH,  // Read short LZ length
        SHORT_LZ_READ_LENGTH2, // Read alternate length encoding
        SHORT_LZ_READ_DIST,    // Read short LZ distance slot
        SHORT_LZ_READ_EXTRA,   // Read extra distance bits

        // Long LZ states
        LONG_LZ_READ_LENGTH,   // Read long LZ length
        LONG_LZ_READ_PLACE,    // Read long LZ place
        LONG_LZ_READ_PLACE_EXTRA, // Read place extra bits
        LONG_LZ_READ_LOW,      // Read low 8 bits for new distance

        COPY_MATCH,            // Copy match bytes
        DONE                   // Finished
    };

    // Static decode tables
    static constexpr unsigned short_dec_[8] = {
        0x0000, 0x4000, 0x8000, 0xa000, 0xc000, 0xd000, 0xe000, 0xf000
    };
    static constexpr unsigned short_pos_[9] = {0, 0, 0, 1, 2, 3, 4, 5, 6};
    static constexpr unsigned long_dec_[8] = {
        0x0000, 0x4000, 0x6000, 0x8000, 0xa000, 0xb000, 0xc000, 0xd000
    };
    static constexpr unsigned long_pos_[10] = {0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
    static constexpr unsigned hf_dec_[8] = {
        0x0000, 0x8000, 0xc000, 0xe000, 0xf000, 0xf800, 0xfc00, 0xfe00
    };
    static constexpr unsigned hf_pos_[10] = {0, 0, 1, 2, 3, 4, 5, 6, 7, 8};

    // Bit buffer for streaming
    u64 bit_buffer_ = 0;
    unsigned bits_left_ = 0;

    // State machine
    state state_ = state::READ_FLAG_BYTE;

    // Window buffer
    std::vector<u8> window_;
    size_t unp_ptr_ = 0;

    // Distance history
    unsigned old_dist_[4] = {0};
    unsigned old_dist_ptr_ = 0;
    unsigned last_dist_ = 0;
    unsigned last_length_ = 0;

    // Adaptive Huffman tables
    u16 ch_set_[256];    // Main character set
    u16 ch_set_b_[256];  // Distance place set

    // Place/frequency tracking
    u8 place_[256] = {0};
    u8 place_b_[256] = {0};
    u8 place_c_[256] = {0};
    unsigned ntopl_[16] = {0};
    unsigned ntopl_b_[16] = {0};
    unsigned ntopl_c_[16] = {0};

    // Decoding state
    bool stmode_ = false;
    unsigned lcount_ = 0;
    unsigned flags_cnt_ = 0;
    unsigned flag_buf_ = 0;
    unsigned avr_ln1_ = 0;
    unsigned avr_ln2_ = 0;
    unsigned avr_ln3_ = 0;
    unsigned avr_plc_ = 0;
    unsigned avr_plc_b_ = 0;
    unsigned num_huf_ = 0;
    unsigned nhfb_ = 0;
    unsigned max_dist3_ = 0x2001;

    // Current operation state (for resuming)
    unsigned cur_length_ = 0;
    unsigned cur_distance_ = 0;
    unsigned cur_slot_ = 0;
    unsigned cur_pos_ = 0;
    unsigned cur_hf_idx_ = 0;
    unsigned match_remaining_ = 0;
    bool initialized_ = false;
};

}  // namespace crate
