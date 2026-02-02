#pragma once

#include <crate/core/decompressor.hh>
#include <array>
#include <vector>

namespace crate {

// StuffIt Method 15: Arsenic (BWT + Arithmetic coding + MTF)
// Based on XADStuffItArsenicHandle from XADMaster
class CRATE_EXPORT stuffit_arsenic_decompressor : public decompressor {
public:
    stuffit_arsenic_decompressor();

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    [[nodiscard]] bool supports_streaming() const override { return true; }

    void reset() override;

private:
    static constexpr size_t MAX_BLOCK_SIZE = 1 << 24;  // 16MB max

    // Arithmetic decoder model
    struct arsenic_symbol {
        int symbol;
        int frequency;
    };

    struct arsenic_model {
        int total_frequency = 0;
        int increment = 0;
        int frequency_limit = 0;
        size_t num_symbols = 0;
        std::array<arsenic_symbol, 128> symbols{};

        void init(int first_sym, int last_sym, int inc, int freq_limit);
        void reset_frequencies();
        void increase_frequency(size_t idx);
    };

    // Move-to-Front decoder
    class mtf_decoder {
    public:
        mtf_decoder();
        void reset();
        u8 decode(size_t symbol);
    private:
        std::array<u8, 256> table_;
    };

    // Bit buffer for arithmetic decoder
    void refill_bit_buffer();
    int read_bit_be();

    // Input tracking for streaming
    const byte* in_ptr_ = nullptr;
    const byte* in_end_ = nullptr;

    // Arithmetic decoder operations
    int decode_symbol(arsenic_model& model);
    int decode_bit_string(arsenic_model& model, int bits);
    void read_next_code(i32 sym_low, i32 sym_size, i32 sym_tot);

    // BWT inverse
    void calculate_inverse_bwt();

    // Randomization table
    static const u16 RandomizationTable[256];

    // State machine
    enum class state : u8 {
        INIT_DECODER,
        READ_HEADER_A,
        READ_HEADER_S,
        READ_BLOCK_BITS,
        READ_END_FLAG,
        READ_BLOCK_HEADER,
        READ_BLOCK_DATA,
        PROCESS_BLOCK,
        OUTPUT_BLOCK,
        READ_CRC,
        DONE
    };

    state state_ = state::INIT_DECODER;

    // Bit buffer (MSB first for arithmetic coding)
    u32 bit_buffer_ = 0;
    unsigned bits_in_buffer_ = 0;

    // Arithmetic decoder state
    static constexpr i32 NUM_BITS = 26;
    static constexpr i32 ONE = 1 << (NUM_BITS - 1);
    static constexpr i32 HALF = 1 << (NUM_BITS - 2);
    i32 range_ = 0;
    i32 code_ = 0;
    int init_bits_read_ = 0;
    bool decoder_initialized_ = false;

    // Models
    arsenic_model initial_model_;
    arsenic_model selector_model_;
    std::array<arsenic_model, 7> mtf_models_;
    mtf_decoder mtf_;

    // Block parameters
    int block_bits_ = 0;
    size_t block_size_ = 0;
    bool end_of_blocks_ = false;

    // Current block state
    bool randomized_ = false;
    size_t transform_index_ = 0;
    size_t num_bytes_ = 0;
    std::vector<u8> block_;
    std::vector<u32> transform_;

    // Block reading state
    int selector_value_ = -1;
    size_t zero_state_ = 0;
    size_t zero_count_ = 0;
    bool in_zero_run_ = false;

    // Output state
    size_t output_pos_ = 0;
    size_t rand_index_ = 0;
    int rand_count_ = 0;
    int byte_count_ = 0;
    int count_state_ = 0;
    int last_byte_ = 0;
    int repeat_remaining_ = 0;

    // Header reading state
    int header_char_ = 0;
    int bit_string_bits_remaining_ = 0;
    int bit_string_value_ = 0;
};

}  // namespace crate
