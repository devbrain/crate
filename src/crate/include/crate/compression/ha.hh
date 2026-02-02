#pragma once

#include <array>
#include <crate/core/decompressor.hh>
#include <crate/core/types.hh>
#include <vector>

namespace crate {

// =============================================================================
// Streaming Arithmetic Decoder (16-bit range coder with E3 underflow scaling)
// =============================================================================
class ha_arithmetic_decoder {
public:
    static constexpr u16 RANGE_MAX = 0xFFFF;
    static constexpr u16 MSB_MASK = 0x8000;
    static constexpr u16 UNDERFLOW_MASK = 0x4000;

    void reset();

    // Returns threshold value for decoding (before range narrowing)
    // Returns false if not enough input data
    // If end_of_stream is true, pads with zeros instead of failing
    bool try_threshold_val(const byte*& ptr, const byte* end, u16 total, u16& out, bool end_of_stream = false);

    // Update range after decoding a symbol
    // Returns false if not enough input for renormalization
    // If end_of_stream is true, pads with zeros instead of failing
    bool try_decode_update(const byte*& ptr, const byte* end, u16 cum_low, u16 cum_high, u16 total, bool end_of_stream = false);

private:
    bool read_bit(const byte*& ptr, const byte* end, u16& out, bool end_of_stream);
    bool renormalize(const byte*& ptr, const byte* end, bool end_of_stream);

    u16 high_ = RANGE_MAX;
    u16 low_ = 0;
    u16 code_ = 0;
    u8 byte_buffer_ = 0;
    u8 bits_remaining_ = 0;
    bool bootstrapped_ = false;
};

// =============================================================================
// Binary Tree Frequency Table for ASC
// =============================================================================
class ha_binary_tree_table {
public:
    ha_binary_tree_table() = default;
    ha_binary_tree_table(size_t leaf_count, u16 initial_value);

    void init(size_t leaf_count, u16 initial_value);

    [[nodiscard]] u16 root_sum() const { return storage_[1]; }
    [[nodiscard]] u16 symbol_freq(size_t symbol) const;
    [[nodiscard]] std::pair<size_t, u16> navigate_to_symbol(u16 threshold) const;

    void add_frequency(size_t symbol, u16 step, u16 max_total);
    void remove_symbol(size_t symbol);

private:
    void recompute_internals();
    void halve_all();

    size_t leaf_count_ = 0;
    std::vector<u16> storage_;
};

// =============================================================================
// ASC Decompressor (LZ77 + Arithmetic Coding) - Streaming
// =============================================================================
class CRATE_EXPORT ha_asc_decompressor : public decompressor {
public:
    static constexpr u16 WINDOW_CAPACITY = 31200;
    static constexpr u16 SHORT_LEN_COUNT = 16;
    static constexpr u16 LONG_LEN_COUNT = 48;
    static constexpr u16 LONG_LEN_BITS = 4;
    static constexpr u16 LONG_LEN_RANGE = 16;
    static constexpr u16 TOTAL_LENGTHS = SHORT_LEN_COUNT + LONG_LEN_COUNT * LONG_LEN_RANGE;
    static constexpr size_t LEN_TABLE_SIZE = SHORT_LEN_COUNT + LONG_LEN_COUNT;
    static constexpr size_t CHAR_TABLE_SIZE = 256;
    static constexpr size_t POS_TABLE_SIZE = 16;
    static constexpr u16 LEN_STEP = 8;
    static constexpr u16 POS_STEP = 24;
    static constexpr u16 TYPE_STEP = 40;
    static constexpr u16 TOTAL_MAX = 6000;
    static constexpr u16 CHAR_FREQ_MAX = 1000;
    static constexpr size_t CHAR_LOCALITY = 8;
    static constexpr size_t LEN_LOCALITY = 4;
    static constexpr size_t TYPE_CONTEXTS = 4;
    static constexpr u16 MIN_MATCH = 3;

    ha_asc_decompressor();

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    [[nodiscard]] bool supports_streaming() const override { return true; }

    void reset() override;

private:
    void init_state();

    // State machine states
    enum class state : u8 {
        DECODE_TYPE,           // Decoding literal/match/end type
        DECODE_LITERAL,        // Decoding literal character
        EXPAND_POS_TABLE,      // Expanding position table if needed
        DECODE_POSITION,       // Decoding match position
        DECODE_POS_EXTRA,      // Decoding position extra bits
        DECODE_LENGTH,         // Decoding match length
        DECODE_LEN_EXTRA,      // Decoding length extra bits
        COPY_MATCH,            // Copying match bytes
        DONE                   // Finished
    };

    // Helper functions for type/literal/position/length decoding
    void record_literal();
    void record_match();
    void scale_type_context();

    // Arithmetic coder
    ha_arithmetic_decoder coder_;

    // Frequency tables
    ha_binary_tree_table char_main_;
    ha_binary_tree_table char_escape_;
    ha_binary_tree_table len_main_;
    ha_binary_tree_table len_escape_;
    ha_binary_tree_table pos_table_;

    // Type context model
    std::array<std::array<u16, 2>, TYPE_CONTEXTS> type_frequencies_{};
    size_t type_context_ = 0;

    // Escape weights
    u16 char_escape_weight_ = 1;
    u16 len_escape_weight_ = LEN_STEP;
    u16 pos_codes_active_ = 1;
    u16 pos_max_value_ = 1;
    u16 bytes_emitted_ = 0;

    // Window
    std::vector<u8> window_;
    size_t write_pos_ = 0;

    // State machine
    state state_ = state::DECODE_TYPE;

    // Decoding state
    u16 decoded_symbol_ = 0;
    u16 match_position_ = 0;
    u16 match_length_ = 0;
    u16 match_remaining_ = 0;

    // Sub-state for multi-step decodes
    enum class decode_phase : u8 {
        THRESHOLD,
        MAIN_NAVIGATE,
        MAIN_UPDATE,
        ESCAPE_UPDATE,
        ESCAPE_NAVIGATE,
        ESCAPE_SYMBOL_UPDATE,
        LOCALITY_BOOST,
        FINAL_UPDATE
    };
    decode_phase phase_ = decode_phase::THRESHOLD;

    // Temporary values during decode
    u16 threshold_ = 0;
    u16 main_total_ = 0;
    u16 combined_total_ = 0;
    size_t nav_symbol_ = 0;
    u16 nav_lt_ = 0;
    u16 nav_freq_ = 0;
    size_t code_value_ = 0;
    u16 raw_length_ = 0;
};

// =============================================================================
// HSC Decompressor (PPM + Arithmetic Coding) - Streaming
// =============================================================================
class CRATE_EXPORT ha_hsc_decompressor : public decompressor {
public:
    static constexpr size_t MAX_ORDER = 4;
    static constexpr size_t CONTEXT_POOL_SIZE = 10000;
    static constexpr size_t FREQ_BLOCK_POOL_SIZE = 32760;
    static constexpr size_t HASH_TABLE_SIZE = 16384;
    static constexpr u16 LOW_FREQ_THRESHOLD = 3;
    static constexpr u16 MAX_TOTAL_FREQ = 8000;
    static constexpr u8 RESCALE_FACTOR_INIT = 4;
    static constexpr u8 ESCAPE_COUNTER_LIMIT = 32;
    static constexpr u8 NON_ESCAPE_THRESHOLD = 5;
    static constexpr u8 NON_ESCAPE_MAX = 10;
    static constexpr u16 NON_ESCAPE_TOTAL_LIMIT = 4;
    static constexpr u16 NULL_PTR = 0xFFFF;
    static constexpr u16 ESCAPE_SYMBOL = 256;

    ha_hsc_decompressor();

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    [[nodiscard]] bool supports_streaming() const override { return true; }

    void reset() override;

private:
    void init_state();
    void build_hash_table();

    [[nodiscard]] u16 compute_hash(const std::array<u8, 4>& bytes, size_t length) const;
    void advance_context(u8 ctx_byte);
    void promote_to_front(u16 ctx_id);
    void prepare_context_search();
    u16 find_longest_context();
    u16 find_next_context();
    [[nodiscard]] bool context_matches(size_t idx, int order) const;
    [[nodiscard]] u16 calculate_escape_probability(u16 low_freq_count, u16 ctx_id) const;
    void reclaim_blocks();
    u16 allocate_context(u8 order, u8 first_char);
    void update_model(u8 character);
    void update_context_stats(size_t idx, u16 block);

    // State machine states
    enum class state : u8 {
        FIND_CONTEXT,          // Finding longest matching context
        DECODE_FROM_CONTEXT,   // Decoding from current context
        DECODE_UNIFORM,        // Decoding uniformly (no context)
        UPDATE_MODEL,          // Updating PPM model
        ALLOCATE_CONTEXTS,     // Allocating new contexts
        OUTPUT_CHAR,           // Outputting decoded character
        DONE                   // Finished
    };

    // Arithmetic coder
    ha_arithmetic_decoder coder_;

    // Context window
    std::array<u8, 4> context_window_{};

    // Hash tables
    std::vector<u16> hash_heads_;
    std::vector<u16> hash_chain_;
    std::vector<u16> hash_rand_;

    // Context pool
    std::vector<std::array<u8, 4>> ctx_bytes_;
    std::vector<u8> ctx_length_;
    std::vector<u8> ctx_char_count_;
    std::vector<u16> ctx_total_freq_;
    std::vector<u8> ctx_low_freq_count_;
    std::vector<u8> ctx_rescale_factor_;

    // LRU list
    std::vector<u16> lru_prev_;
    std::vector<u16> lru_next_;
    u16 lru_front_ = 0;
    u16 lru_back_ = 0;

    // Frequency blocks
    std::vector<u16> freq_value_;
    std::vector<u8> freq_char_;
    std::vector<u16> freq_next_;
    u16 free_block_head_ = 0;
    u16 reclaim_cursor_ = 0;

    // Exclusion tracking
    std::array<bool, 256> excluded_{};
    std::vector<u8> excluded_stack_;

    // Non-escape counter
    u8 non_escape_count_ = 0;
    std::array<u8, MAX_ORDER + 1> initial_escape_{};

    // Update tracking
    size_t update_depth_ = 0;
    std::array<u16, MAX_ORDER + 1> update_contexts_{};
    std::array<u16, MAX_ORDER + 1> update_blocks_{};

    // Order search state
    std::array<u16, MAX_ORDER + 1> order_hashes_{};
    i16 search_order_ = 0;

    // Order reduction
    i16 order_reduction_counter_ = 0;
    u8 current_max_order_ = 0;

    // State machine
    state state_ = state::FIND_CONTEXT;

    // Decode state
    u16 current_ctx_id_ = NULL_PTR;
    u8 min_order_ = 0;
    u8 max_order_ = 0;
    u16 decoded_symbol_ = 0;
    u8 decoded_char_ = 0;

    // Sub-state for multi-step decodes
    enum class decode_phase : u8 {
        THRESHOLD,
        NAVIGATE,
        UPDATE,
        ESCAPE_MASK,
        UNIFORM_SCAN
    };
    decode_phase phase_ = decode_phase::THRESHOLD;

    // Temporary values
    u16 threshold_ = 0;
    u16 total_ = 0;
    u16 escape_ = 0;
    u16 low_count_ = 0;
    u8 scale_ = 0;
    u16 block_ = 0;
    u16 cumulative_ = 0;
    u16 symbol_freq_ = 0;
    u16 symbol_ = 0;
};

}  // namespace crate
