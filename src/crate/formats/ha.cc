#include <crate/formats/ha.hh>
#include <crate/compression/ha.hh>
#include <crate/core/crc.hh>
#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace crate {
    namespace ha {
        // =============================================================================
        // Arithmetic Decoder (16-bit range coder with E3 underflow scaling)
        // =============================================================================
        class arithmetic_decoder {
            public:
                static constexpr u16 RANGE_MAX = 0xFFFF;
                static constexpr u16 MSB_MASK = 0x8000;
                static constexpr u16 UNDERFLOW_MASK = 0x4000;

                explicit arithmetic_decoder(byte_span data)
                    : data_(data), pos_(0) {
                    // Bootstrap: read initial 16-bit code value (big-endian)
                    if (data_.size() >= 2) {
                        code_ = (static_cast <u16>(data_[0]) << 8) | data_[1];
                        pos_ = 2;
                    }
                }

                [[nodiscard]] u16 threshold_val(u16 total) const {
                    u32 range = static_cast <u32>(high_ - low_) + 1;
                    u32 offset = static_cast <u32>(code_ - low_) + 1;
                    return static_cast <u16>((offset * total - 1) / range);
                }

                void decode_update(u16 cum_low, u16 cum_high, u16 total) {
                    u32 range = static_cast <u32>(high_ - low_) + 1;
                    u32 scale = total;

                    u16 new_high = low_ + static_cast <u16>((range * cum_high / scale) - 1);
                    u16 new_low = low_ + static_cast <u16>(range * cum_low / scale);

                    high_ = new_high;
                    low_ = new_low;

                    renormalize();
                }

            private:
                u16 read_bit() {
                    if (bits_remaining_ == 0) {
                        byte_buffer_ = (pos_ < data_.size()) ? data_[pos_++] : 0;
                        bits_remaining_ = 8;
                    }
                    bits_remaining_--;
                    return (byte_buffer_ >> bits_remaining_) & 1;
                }

                void renormalize() {
                    while (true) {
                        if ((high_ ^ low_) & MSB_MASK) {
                            // MSBs don't match
                            if ((low_ & UNDERFLOW_MASK) && !(high_ & UNDERFLOW_MASK)) {
                                // E3 underflow: low = 01..., high = 10...
                                handle_underflow();
                            } else {
                                break;
                            }
                        } else {
                            // MSBs match
                            shift_out_msb();
                        }
                    }
                }

                void shift_out_msb() {
                    low_ <<= 1;
                    high_ = (high_ << 1) | 1;
                    code_ = (code_ << 1) | read_bit();
                }

                void handle_underflow() {
                    low_ = (low_ << 1) & 0x7FFF;
                    high_ = (high_ << 1) | 0x8001;
                    code_ = ((code_ << 1) ^ MSB_MASK) | read_bit();
                }

                byte_span data_;
                size_t pos_ = 0;
                u16 high_ = RANGE_MAX;
                u16 low_ = 0;
                u16 code_ = 0;
                u8 byte_buffer_ = 0;
                u8 bits_remaining_ = 0;
        };

        // =============================================================================
        // Binary Tree Frequency Table for ASC
        // =============================================================================
        class binary_tree_table {
            public:
                binary_tree_table(size_t leaf_count, u16 initial_value)
                    : leaf_count_(leaf_count), storage_(leaf_count * 2, 0) {
                    // Set leaf values
                    for (size_t i = leaf_count_; i < 2 * leaf_count_; i++) {
                        storage_[i] = initial_value;
                    }
                    recompute_internals();
                }

                [[nodiscard]] u16 root_sum() const { return storage_[1]; }

                [[nodiscard]] u16 symbol_freq(size_t symbol) const {
                    return storage_[leaf_count_ + symbol];
                }

                [[nodiscard]] std::pair <size_t, u16> navigate_to_symbol(u16 threshold) const {
                    size_t node = 2;
                    u16 cumulative = 0;

                    while (true) {
                        u16 left_sum = storage_[node];
                        if (cumulative + left_sum <= threshold) {
                            cumulative += left_sum;
                            node++;
                        }
                        if (node >= leaf_count_) {
                            return {node - leaf_count_, cumulative};
                        }
                        node <<= 1;
                    }
                }

                void add_frequency(size_t symbol, u16 step, u16 max_total) {
                    size_t idx = symbol + leaf_count_;
                    while (idx > 0) {
                        storage_[idx] += step;
                        idx >>= 1;
                    }
                    if (storage_[1] >= max_total) {
                        halve_all();
                    }
                }

                void remove_symbol(size_t symbol) {
                    size_t idx = symbol + leaf_count_;
                    u16 amount = storage_[idx];
                    while (idx > 0) {
                        storage_[idx] -= amount;
                        idx >>= 1;
                    }
                }

            private:
                void recompute_internals() {
                    size_t src = (leaf_count_ << 1) - 2;
                    for (size_t dest = leaf_count_ - 1; dest >= 1; dest--) {
                        storage_[dest] = storage_[src] + storage_[src + 1];
                        src -= 2;
                    }
                }

                void halve_all() {
                    for (size_t i = leaf_count_; i < 2 * leaf_count_; i++) {
                        if (storage_[i] > 1) {
                            storage_[i] >>= 1;
                        }
                    }
                    recompute_internals();
                }

                size_t leaf_count_;
                std::vector <u16> storage_;
        };

        // =============================================================================
        // ASC Decoder (LZ77 + Arithmetic Coding)
        // =============================================================================
        class asc_decoder {
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

                explicit asc_decoder(byte_span data)
                    : coder_(data),
                      char_main_(CHAR_TABLE_SIZE, 0),
                      char_escape_(CHAR_TABLE_SIZE, 1),
                      len_main_(LEN_TABLE_SIZE, 0),
                      len_escape_(LEN_TABLE_SIZE, 1),
                      pos_table_(POS_TABLE_SIZE, 0),
                      window_(WINDOW_CAPACITY, 0) {
                    // Initialize type model
                    for (auto& ctx : type_frequencies_) {
                        ctx[0] = TYPE_STEP;
                        ctx[1] = TYPE_STEP;
                    }
                    pos_table_.add_frequency(0, POS_STEP, TOTAL_MAX);
                }

                result_t <byte_vector> decompress() {
                    byte_vector output;

                    while (true) {
                        u16 type_total = type_frequencies_[type_context_][0] + type_frequencies_[type_context_][1];
                        u16 threshold = coder_.threshold_val(type_total + 1);
                        u16 lit_freq = type_frequencies_[type_context_][0];

                        if (lit_freq > threshold) {
                            // Literal
                            coder_.decode_update(0, lit_freq, type_total + 1);
                            record_literal();

                            auto byte_result = decode_character();
                            if (!byte_result) return std::unexpected(byte_result.error());

                            u8 byte = *byte_result;
                            output.push_back(byte);
                            window_[write_pos_++] = byte;
                            if (write_pos_ >= WINDOW_CAPACITY) write_pos_ = 0;

                            if (bytes_emitted_ < WINDOW_CAPACITY) bytes_emitted_++;
                        } else if (type_total > threshold) {
                            // Match
                            coder_.decode_update(lit_freq, type_total, type_total + 1);
                            record_match();

                            // Expand position table if needed
                            while (bytes_emitted_ > pos_max_value_) {
                                pos_table_.add_frequency(pos_codes_active_, POS_STEP, TOTAL_MAX);
                                pos_codes_active_++;
                                pos_max_value_ <<= 1;
                            }

                            auto pos_result = decode_position();
                            if (!pos_result) return std::unexpected(pos_result.error());
                            u16 position = *pos_result;

                            auto len_result = decode_length();
                            if (!len_result) return std::unexpected(len_result.error());
                            u16 length = *len_result;

                            // Copy from window
                            copy_from_window(position, length, output);

                            if (bytes_emitted_ < WINDOW_CAPACITY) {
                                bytes_emitted_ += length;
                                if (bytes_emitted_ > WINDOW_CAPACITY) {
                                    bytes_emitted_ = WINDOW_CAPACITY;
                                }
                            }
                        } else {
                            // End of stream
                            coder_.decode_update(type_total, type_total + 1, type_total + 1);
                            break;
                        }
                    }

                    return output;
                }

            private:
                void record_literal() {
                    u16 total = type_frequencies_[type_context_][0] + type_frequencies_[type_context_][1];
                    type_frequencies_[type_context_][0] += TYPE_STEP;
                    if (total >= TOTAL_MAX) {
                        scale_type_context();
                    }
                    type_context_ = (type_context_ << 1) & 0x3;
                }

                void record_match() {
                    u16 total = type_frequencies_[type_context_][0] + type_frequencies_[type_context_][1];
                    type_frequencies_[type_context_][1] += TYPE_STEP;
                    if (total >= TOTAL_MAX) {
                        scale_type_context();
                    }
                    type_context_ = ((type_context_ << 1) | 1) & 0x3;
                }

                void scale_type_context() {
                    type_frequencies_[type_context_][0] = std::max(
                        u16(1), u16(type_frequencies_[type_context_][0] >> 1));
                    type_frequencies_[type_context_][1] = std::max(
                        u16(1), u16(type_frequencies_[type_context_][1] >> 1));
                }

                result_t <u8> decode_character() {
                    u16 main_total = char_main_.root_sum();
                    u16 combined = main_total + char_escape_weight_;
                    u16 threshold = coder_.threshold_val(combined);

                    size_t symbol;
                    if (threshold >= main_total) {
                        // Escape to new character
                        coder_.decode_update(main_total, combined, combined);

                        u16 esc_total = char_escape_.root_sum();
                        u16 esc_threshold = coder_.threshold_val(esc_total);
                        auto [sym, lt] = char_escape_.navigate_to_symbol(esc_threshold);
                        u16 freq = char_escape_.symbol_freq(sym);
                        coder_.decode_update(lt, lt + freq, esc_total);

                        char_escape_.remove_symbol(sym);

                        if (char_escape_.root_sum() != 0) {
                            char_escape_weight_++;
                        } else {
                            char_escape_weight_ = 0;
                        }

                        // Boost locality
                        size_t start = (sym > CHAR_LOCALITY) ? sym - CHAR_LOCALITY : 0;
                        size_t end = std::min(sym + CHAR_LOCALITY, CHAR_TABLE_SIZE - 1);
                        for (size_t i = start; i < end; i++) {
                            if (char_escape_.symbol_freq(i) > 0) {
                                char_escape_.add_frequency(i, 1, CHAR_FREQ_MAX);
                            }
                        }
                        symbol = sym;
                    } else {
                        auto [sym, lt] = char_main_.navigate_to_symbol(threshold);
                        u16 freq = char_main_.symbol_freq(sym);
                        coder_.decode_update(lt, lt + freq, combined);
                        symbol = sym;
                    }

                    char_main_.add_frequency(symbol, 1, CHAR_FREQ_MAX);

                    if (char_main_.symbol_freq(symbol) == 3) {
                        if (char_escape_weight_ > 1) {
                            char_escape_weight_--;
                        }
                    }

                    return static_cast <u8>(symbol);
                }

                result_t <u16> decode_position() {
                    u16 total = pos_table_.root_sum();
                    u16 threshold = coder_.threshold_val(total);
                    auto [code, lt] = pos_table_.navigate_to_symbol(threshold);
                    u16 freq = pos_table_.symbol_freq(code);
                    coder_.decode_update(lt, lt + freq, total);

                    pos_table_.add_frequency(code, POS_STEP, TOTAL_MAX);

                    u16 position;
                    if (code > 1) {
                        u16 base = 1u << (code - 1);
                        u16 range = (base == (pos_max_value_ >> 1))
                                        ? bytes_emitted_ - (pos_max_value_ >> 1)
                                        : base;

                        u16 extra = coder_.threshold_val(range);
                        coder_.decode_update(extra, extra + 1, range);
                        position = extra + base;
                    } else {
                        position = static_cast <u16>(code);
                    }

                    return position;
                }

                result_t <u16> decode_length() {
                    u16 main_total = len_main_.root_sum();
                    u16 combined = main_total + len_escape_weight_;
                    u16 threshold = coder_.threshold_val(combined);

                    size_t code;
                    if (threshold >= main_total) {
                        // Escape
                        coder_.decode_update(main_total, combined, combined);

                        u16 esc_total = len_escape_.root_sum();
                        u16 esc_threshold = coder_.threshold_val(esc_total);
                        auto [sym, lt] = len_escape_.navigate_to_symbol(esc_threshold);
                        u16 freq = len_escape_.symbol_freq(sym);
                        coder_.decode_update(lt, lt + freq, esc_total);

                        len_escape_.remove_symbol(sym);

                        if (len_escape_.root_sum() != 0) {
                            len_escape_weight_ += LEN_STEP;
                        } else {
                            len_escape_weight_ = 0;
                        }

                        // Boost locality
                        size_t start = (sym > LEN_LOCALITY) ? sym - LEN_LOCALITY : 0;
                        size_t end = std::min(sym + LEN_LOCALITY, LEN_TABLE_SIZE - 1);
                        for (size_t i = start; i < end; i++) {
                            if (len_escape_.symbol_freq(i) > 0) {
                                len_escape_.add_frequency(i, 1, TOTAL_MAX);
                            }
                        }
                        code = sym;
                    } else {
                        auto [sym, lt] = len_main_.navigate_to_symbol(threshold);
                        u16 freq = len_main_.symbol_freq(sym);
                        coder_.decode_update(lt, lt + freq, combined);
                        code = sym;
                    }

                    len_main_.add_frequency(code, LEN_STEP, TOTAL_MAX);

                    if (len_main_.symbol_freq(code) == 3 * LEN_STEP) {
                        if (len_escape_weight_ > LEN_STEP) {
                            len_escape_weight_ -= LEN_STEP;
                        }
                    }

                    u16 raw_length;
                    if (code == SHORT_LEN_COUNT - 1) {
                        raw_length = TOTAL_LENGTHS - 1;
                    } else if (code >= SHORT_LEN_COUNT) {
                        u16 extra = coder_.threshold_val(LONG_LEN_RANGE);
                        coder_.decode_update(extra, extra + 1, LONG_LEN_RANGE);
                        u32 base = static_cast <u32>(code - SHORT_LEN_COUNT);
                        u32 computed = (base << LONG_LEN_BITS) + extra + SHORT_LEN_COUNT - 1;
                        raw_length = static_cast <u16>(computed);
                    } else {
                        raw_length = static_cast <u16>(code);
                    }

                    return raw_length + MIN_MATCH;
                }

                void copy_from_window(u16 offset, u16 length, byte_vector& output) {
                    size_t capacity = window_.size();
                    size_t read_pos = (write_pos_ > offset)
                                          ? write_pos_ - 1 - offset
                                          : capacity - 1 - offset + write_pos_;

                    for (u16 i = 0; i < length; i++) {
                        u8 byte = window_[read_pos];
                        output.push_back(byte);
                        window_[write_pos_] = byte;

                        write_pos_++;
                        if (write_pos_ >= capacity) write_pos_ = 0;
                        read_pos++;
                        if (read_pos >= capacity) read_pos = 0;
                    }
                }

                arithmetic_decoder coder_;
                binary_tree_table char_main_;
                binary_tree_table char_escape_;
                binary_tree_table len_main_;
                binary_tree_table len_escape_;
                binary_tree_table pos_table_;

                std::array <std::array <u16, 2>, TYPE_CONTEXTS> type_frequencies_{};
                size_t type_context_ = 0;

                u16 char_escape_weight_ = 1;
                u16 len_escape_weight_ = LEN_STEP;
                u16 pos_codes_active_ = 1;
                u16 pos_max_value_ = 1;
                u16 bytes_emitted_ = 0;

                byte_vector window_;
                size_t write_pos_ = 0;
        };

        inline result_t <byte_vector> decompress_asc(byte_span data) {
            asc_decoder decoder(data);
            return decoder.decompress();
        }

        // =============================================================================
        // HSC Decoder (PPM + Arithmetic Coding) - Order-4 Context Model
        // =============================================================================
        class hsc_decoder {
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

                explicit hsc_decoder(byte_span data)
                    : coder_(data) {
                    // Initialize hash table
                    hash_heads_.assign(HASH_TABLE_SIZE, NULL_PTR);
                    hash_chain_.assign(CONTEXT_POOL_SIZE, 0);

                    // Initialize LRU list
                    lru_prev_.resize(CONTEXT_POOL_SIZE);
                    lru_next_.resize(CONTEXT_POOL_SIZE);
                    for (size_t i = 0; i < CONTEXT_POOL_SIZE; i++) {
                        lru_next_[i] = static_cast <u16>(i + 1);
                        lru_prev_[i] = static_cast <u16>(i - 1);
                    }
                    lru_front_ = 0;
                    lru_back_ = static_cast <u16>(CONTEXT_POOL_SIZE - 1);

                    // Initialize context pool
                    ctx_bytes_.resize(CONTEXT_POOL_SIZE);
                    ctx_length_.assign(CONTEXT_POOL_SIZE, 0xFF);
                    ctx_char_count_.assign(CONTEXT_POOL_SIZE, 0);
                    ctx_total_freq_.assign(CONTEXT_POOL_SIZE, 0);
                    ctx_low_freq_count_.assign(CONTEXT_POOL_SIZE, 0);
                    ctx_rescale_factor_.assign(CONTEXT_POOL_SIZE, 0);

                    // Initialize frequency block pool
                    freq_value_.assign(FREQ_BLOCK_POOL_SIZE, 0);
                    freq_char_.assign(FREQ_BLOCK_POOL_SIZE, 0);
                    freq_next_.assign(FREQ_BLOCK_POOL_SIZE, NULL_PTR);

                    for (size_t i = CONTEXT_POOL_SIZE; i < FREQ_BLOCK_POOL_SIZE - 1; i++) {
                        freq_next_[i] = static_cast <u16>(i + 1);
                    }
                    free_block_head_ = static_cast <u16>(CONTEXT_POOL_SIZE);

                    // Build hash randomization table
                    build_hash_table();

                    // Initialize escape tracking
                    initial_escape_[0] = ESCAPE_COUNTER_LIMIT >> 1;
                    for (size_t i = 1; i <= MAX_ORDER; i++) {
                        initial_escape_[i] = (ESCAPE_COUNTER_LIMIT >> 1) - 1;
                    }

                    order_reduction_counter_ = static_cast <i16>(CONTEXT_POOL_SIZE / 4);
                    current_max_order_ = static_cast <u8>(MAX_ORDER);
                }

                result_t <byte_vector> decompress() {
                    byte_vector output;

                    while (true) {
                        u16 ctx_id = find_longest_context();

                        u8 min_order = (ctx_id == NULL_PTR) ? 0 : ctx_length_[ctx_id] + 1;
                        u8 max_order = current_max_order_ + 1;

                        u16 decoded;
                        while (true) {
                            if (ctx_id == NULL_PTR) {
                                auto r = decode_uniform();
                                if (!r) return std::unexpected(r.error());
                                decoded = *r;
                                break;
                            }

                            auto r = decode_from_context(ctx_id);
                            if (!r) return std::unexpected(r.error());

                            if (*r != ESCAPE_SYMBOL) {
                                promote_to_front(ctx_id);
                                decoded = *r;
                                break;
                            }

                            ctx_id = find_next_context();
                        }

                        if (decoded == ESCAPE_SYMBOL) break;

                        u8 character = static_cast <u8>(decoded);
                        update_model(character);

                        while (max_order > min_order) {
                            max_order--;
                            allocate_context(max_order, character);
                        }

                        output.push_back(character);
                        advance_context(character);
                    }

                    return output;
                }

            private:
                void build_hash_table() {
                    hash_rand_.resize(HASH_TABLE_SIZE);
                    i64 seed = 10;

                    for (size_t i = 0; i < HASH_TABLE_SIZE; i++) {
                        i64 quotient = seed / (2147483647LL / 16807);
                        i64 remainder = seed % (2147483647LL / 16807);
                        i64 product = 16807LL * remainder - (2147483647LL % 16807) * quotient;
                        seed = (product > 0) ? product : product + 2147483647LL;
                        hash_rand_[i] = static_cast <u16>(seed) & static_cast <u16>(HASH_TABLE_SIZE - 1);
                    }
                }

                [[nodiscard]] u16 compute_hash(const std::array <u8, 4>& bytes, size_t length) const {
                    u16 mask = static_cast <u16>(HASH_TABLE_SIZE - 1);
                    u16 h = 0;
                    for (size_t i = 0; i < std::min(length, size_t(4)); i++) {
                        h = hash_rand_[((bytes[i] + h) & mask)];
                    }
                    return h;
                }

                void advance_context(u8 byte) {
                    context_window_[3] = context_window_[2];
                    context_window_[2] = context_window_[1];
                    context_window_[1] = context_window_[0];
                    context_window_[0] = byte;
                }

                void promote_to_front(u16 ctx_id) {
                    if (ctx_id == lru_front_) return;

                    size_t idx = ctx_id;

                    if (ctx_id == lru_back_) {
                        lru_back_ = lru_prev_[idx];
                    } else {
                        u16 next = lru_next_[idx];
                        u16 prev = lru_prev_[idx];
                        lru_prev_[next] = prev;
                        lru_next_[prev] = next;
                    }

                    lru_prev_[lru_front_] = ctx_id;
                    lru_next_[idx] = lru_front_;
                    lru_front_ = ctx_id;
                }

                void prepare_context_search() {
                    u16 mask = static_cast <u16>(HASH_TABLE_SIZE - 1);

                    order_hashes_[1] = hash_rand_[context_window_[0]];
                    order_hashes_[2] = hash_rand_[(context_window_[1] + order_hashes_[1]) & mask];
                    order_hashes_[3] = hash_rand_[(context_window_[2] + order_hashes_[2]) & mask];
                    order_hashes_[4] = hash_rand_[(context_window_[3] + order_hashes_[3]) & mask];

                    update_depth_ = 0;
                    excluded_stack_.clear();
                    std::fill(excluded_.begin(), excluded_.end(), false);
                    search_order_ = static_cast <i16>(MAX_ORDER + 1);
                }

                u16 find_longest_context() {
                    prepare_context_search();
                    return find_next_context();
                }

                u16 find_next_context() {
                    for (int order = search_order_ - 1; order >= 0; order--) {
                        u16 hash = order_hashes_[static_cast <size_t>(order)];
                        u16 ctx_id = hash_heads_[hash];

                        while (ctx_id != NULL_PTR) {
                            size_t idx = ctx_id;

                            if (ctx_length_[idx] == order && context_matches(idx, order)) {
                                search_order_ = static_cast <i16>(order);
                                return ctx_id;
                            }

                            ctx_id = hash_chain_[idx];
                        }
                    }
                    return NULL_PTR;
                }

                [[nodiscard]] bool context_matches(size_t idx, int order) const {
                    switch (order) {
                        case 4: return ctx_bytes_[idx][0] == context_window_[0] &&
                                       ctx_bytes_[idx][1] == context_window_[1] &&
                                       ctx_bytes_[idx][2] == context_window_[2] &&
                                       ctx_bytes_[idx][3] == context_window_[3];
                        case 3: return ctx_bytes_[idx][0] == context_window_[0] &&
                                       ctx_bytes_[idx][1] == context_window_[1] &&
                                       ctx_bytes_[idx][2] == context_window_[2];
                        case 2: return ctx_bytes_[idx][0] == context_window_[0] &&
                                       ctx_bytes_[idx][1] == context_window_[1];
                        case 1: return ctx_bytes_[idx][0] == context_window_[0];
                        case 0: return true;
                        default: return false;
                    }
                }

                [[nodiscard]] u16 calculate_escape_probability(u16 low_freq_count, u16 ctx_id) const {
                    size_t idx = ctx_id;
                    u16 total = ctx_total_freq_[idx];
                    u8 char_count = ctx_char_count_[idx];

                    if (total == 1) {
                        return (initial_escape_[ctx_length_[idx]] >= (ESCAPE_COUNTER_LIMIT >> 1)) ? 2 : 1;
                    }

                    if (char_count == 255) return 1;

                    u16 escape = low_freq_count;

                    if (char_count > 0 && ((static_cast <u16>(char_count) + 1) << 1) >= total) {
                        escape = static_cast <u16>(
                            (static_cast <u32>(escape) * ((static_cast <u32>(char_count) + 1) << 1)) / total);

                        if (static_cast <u16>(char_count) + 1 == total) {
                            escape = static_cast <u16>(escape + ((static_cast <u16>(char_count) + 1) >> 1));
                        }
                    }

                    return std::max(static_cast <u16>(1), escape);
                }

                result_t <u16> decode_without_exclusions(u16 ctx_id) {
                    size_t idx = ctx_id;
                    u16 escape = calculate_escape_probability(ctx_low_freq_count_[idx], ctx_id);
                    u16 total = ctx_total_freq_[idx];

                    u8 scale = 0;
                    u16 threshold;

                    if (non_escape_count_ >= NON_ESCAPE_THRESHOLD) {
                        scale = (total <= NON_ESCAPE_TOTAL_LIMIT && non_escape_count_ == NON_ESCAPE_MAX) ? 2 : 1;
                        total <<= scale;
                        threshold = coder_.threshold_val(total + escape) >> scale;
                    } else {
                        threshold = coder_.threshold_val(total + escape);
                    }

                    u16 block = ctx_id;
                    u16 cumulative = 0;
                    u16 symbol_freq = 0;

                    while (block != NULL_PTR) {
                        u16 freq = freq_value_[block];

                        if (cumulative + freq > threshold) {
                            symbol_freq = freq;
                            if (scale > 0) symbol_freq <<= scale;
                            break;
                        }

                        cumulative += freq;
                        block = freq_next_[block];
                    }

                    if (scale > 0) cumulative <<= scale;

                    update_depth_ = 1;

                    if (block != NULL_PTR) {
                        coder_.decode_update(cumulative, cumulative + symbol_freq, total + escape);

                        if (ctx_total_freq_[idx] == 1 && initial_escape_[ctx_length_[idx]] > 0) {
                            initial_escape_[ctx_length_[idx]]--;
                        }

                        update_blocks_[0] = block;
                        update_contexts_[0] = ctx_id;

                        if (non_escape_count_ < NON_ESCAPE_MAX) non_escape_count_++;

                        return static_cast <u16>(freq_char_[block]);
                    } else {
                        coder_.decode_update(total, total + escape, total + escape);

                        if (ctx_total_freq_[idx] == 1 && initial_escape_[ctx_length_[idx]] < ESCAPE_COUNTER_LIMIT) {
                            initial_escape_[ctx_length_[idx]]++;
                        }

                        u16 blk = ctx_id;
                        u16 last = 0;
                        while (blk != NULL_PTR) {
                            u8 ch = freq_char_[blk];
                            excluded_stack_.push_back(ch);
                            excluded_[ch] = true;
                            last = blk;
                            blk = freq_next_[blk];
                        }

                        update_contexts_[0] = 0x8000 | ctx_id;
                        update_blocks_[0] = last;
                        non_escape_count_ = 0;

                        return ESCAPE_SYMBOL;
                    }
                }

                result_t <u16> decode_with_exclusions(u16 ctx_id) {
                    size_t idx = ctx_id;

                    u16 total = 0;
                    u16 low_count = 0;
                    u16 blk = ctx_id;

                    while (blk != NULL_PTR) {
                        u8 ch = freq_char_[blk];
                        if (!excluded_[ch]) {
                            u16 freq = freq_value_[blk];
                            total += freq;
                            if (freq < LOW_FREQ_THRESHOLD) low_count++;
                        }
                        blk = freq_next_[blk];
                    }

                    u16 escape = calculate_escape_probability(low_count, ctx_id);
                    u16 threshold = coder_.threshold_val(total + escape);

                    u16 block = ctx_id;
                    u16 cumulative = 0;
                    u16 symbol_freq = 0;

                    while (block != NULL_PTR) {
                        u8 ch = freq_char_[block];

                        if (!excluded_[ch]) {
                            u16 freq = freq_value_[block];

                            if (cumulative + freq > threshold) {
                                symbol_freq = freq;
                                break;
                            }

                            cumulative += freq;
                        }

                        block = freq_next_[block];
                    }

                    if (block != NULL_PTR) {
                        coder_.decode_update(cumulative, cumulative + symbol_freq, total + escape);

                        if (ctx_total_freq_[idx] == 1 && initial_escape_[ctx_length_[idx]] > 0) {
                            initial_escape_[ctx_length_[idx]]--;
                        }

                        update_blocks_[update_depth_] = block;
                        update_contexts_[update_depth_] = ctx_id;
                        update_depth_++;
                        non_escape_count_++;

                        return static_cast <u16>(freq_char_[block]);
                    } else {
                        coder_.decode_update(total, total + escape, total + escape);

                        if (ctx_total_freq_[idx] == 1 && initial_escape_[ctx_length_[idx]] < ESCAPE_COUNTER_LIMIT) {
                            initial_escape_[ctx_length_[idx]]++;
                        }

                        blk = ctx_id;
                        u16 last = 0;
                        while (blk != NULL_PTR) {
                            u8 ch = freq_char_[blk];
                            if (!excluded_[ch]) {
                                excluded_stack_.push_back(ch);
                                excluded_[ch] = true;
                            }
                            last = blk;
                            blk = freq_next_[blk];
                        }

                        update_contexts_[update_depth_] = 0x8000 | ctx_id;
                        update_blocks_[update_depth_] = last;
                        update_depth_++;

                        return ESCAPE_SYMBOL;
                    }
                }

                result_t <u16> decode_from_context(u16 ctx_id) {
                    if (excluded_stack_.empty()) {
                        return decode_without_exclusions(ctx_id);
                    } else {
                        return decode_with_exclusions(ctx_id);
                    }
                }

                result_t <u16> decode_uniform() {
                    u16 unmasked_count = 257 - static_cast <u16>(excluded_stack_.size());
                    u16 threshold = coder_.threshold_val(unmasked_count);

                    u16 symbol = 0;
                    u16 cumulative = 0;

                    while (symbol < 256) {
                        if (excluded_[symbol]) {
                            symbol++;
                            continue;
                        }

                        if (cumulative + 1 > threshold) break;

                        cumulative++;
                        symbol++;
                    }

                    coder_.decode_update(cumulative, cumulative + 1, unmasked_count);
                    return symbol;
                }

                void reclaim_blocks() {
                    while (true) {
                        while (true) {
                            reclaim_cursor_++;
                            if (reclaim_cursor_ >= CONTEXT_POOL_SIZE) reclaim_cursor_ = 0;
                            if (freq_next_[reclaim_cursor_] != NULL_PTR) break;
                        }

                        bool in_stack = false;
                        for (size_t i = 0; i <= update_depth_; i++) {
                            if ((update_contexts_[i] & 0x7FFF) == reclaim_cursor_) {
                                in_stack = true;
                                break;
                            }
                        }

                        if (!in_stack) break;
                    }

                    size_t ctx = reclaim_cursor_;

                    u16 min_freq = freq_value_[ctx];
                    u16 blk = freq_next_[ctx];
                    while (blk != NULL_PTR) {
                        if (freq_value_[blk] < min_freq) min_freq = freq_value_[blk];
                        blk = freq_next_[blk];
                    }
                    min_freq++;

                    if (freq_value_[ctx] < min_freq) {
                        blk = freq_next_[ctx];
                        while (freq_value_[blk] < min_freq && freq_next_[blk] != NULL_PTR) {
                            blk = freq_next_[blk];
                        }

                        freq_value_[ctx] = freq_value_[blk];
                        freq_char_[ctx] = freq_char_[blk];

                        u16 next = freq_next_[blk];
                        freq_next_[blk] = free_block_head_;
                        free_block_head_ = freq_next_[ctx];
                        freq_next_[ctx] = next;

                        if (next == NULL_PTR) {
                            ctx_char_count_[ctx] = 0;
                            ctx_total_freq_[ctx] = freq_value_[ctx];
                            ctx_low_freq_count_[ctx] = (ctx_total_freq_[ctx] < LOW_FREQ_THRESHOLD) ? 1 : 0;
                            return;
                        }
                    }

                    freq_value_[ctx] /= min_freq;
                    ctx_total_freq_[ctx] = freq_value_[ctx];
                    ctx_low_freq_count_[ctx] = (ctx_total_freq_[ctx] < LOW_FREQ_THRESHOLD) ? 1 : 0;
                    ctx_char_count_[ctx] = 0;

                    size_t prev = ctx;
                    blk = freq_next_[prev];

                    while (blk != NULL_PTR) {
                        if (freq_value_[blk] < min_freq) {
                            freq_next_[prev] = freq_next_[blk];
                            freq_next_[blk] = free_block_head_;
                            free_block_head_ = blk;
                            blk = freq_next_[prev];
                        } else {
                            ctx_char_count_[ctx]++;
                            freq_value_[blk] /= min_freq;
                            ctx_total_freq_[ctx] += freq_value_[blk];
                            if (freq_value_[blk] < LOW_FREQ_THRESHOLD) ctx_low_freq_count_[ctx]++;
                            prev = blk;
                            blk = freq_next_[prev];
                        }
                    }
                }

                u16 allocate_context(u8 order, u8 first_char) {
                    u16 new_ctx = lru_back_;
                    lru_back_ = lru_prev_[new_ctx];

                    lru_prev_[lru_front_] = new_ctx;
                    lru_next_[new_ctx] = lru_front_;
                    lru_front_ = new_ctx;

                    size_t idx = new_ctx;

                    if (ctx_length_[idx] != 0xFF) {
                        if (ctx_length_[idx] == MAX_ORDER) {
                            order_reduction_counter_--;
                            if (order_reduction_counter_ == 0) {
                                current_max_order_ = static_cast <u8>(MAX_ORDER - 1);
                            }
                        }

                        u16 hash = compute_hash(ctx_bytes_[idx], ctx_length_[idx]);

                        if (hash_heads_[hash] == new_ctx) {
                            hash_heads_[hash] = hash_chain_[idx];
                        } else {
                            u16 prev = hash_heads_[hash];
                            while (hash_chain_[prev] != new_ctx) {
                                prev = hash_chain_[prev];
                            }
                            hash_chain_[prev] = hash_chain_[idx];
                        }

                        if (freq_next_[idx] != NULL_PTR) {
                            u16 last = freq_next_[idx];
                            while (freq_next_[last] != NULL_PTR) {
                                last = freq_next_[last];
                            }
                            freq_next_[last] = free_block_head_;
                            free_block_head_ = freq_next_[idx];
                        }
                    }

                    freq_next_[idx] = NULL_PTR;
                    ctx_low_freq_count_[idx] = 1;
                    ctx_total_freq_[idx] = 1;
                    freq_value_[idx] = 1;
                    freq_char_[idx] = first_char;
                    ctx_rescale_factor_[idx] = RESCALE_FACTOR_INIT;
                    ctx_char_count_[idx] = 0;
                    ctx_length_[idx] = order;
                    ctx_bytes_[idx] = context_window_;

                    u16 hash = compute_hash(context_window_, order);
                    hash_chain_[idx] = hash_heads_[hash];
                    hash_heads_[hash] = new_ctx;

                    return new_ctx;
                }

                void update_model(u8 character) {
                    while (update_depth_ != 0) {
                        update_depth_--;

                        u16 block = update_blocks_[update_depth_];
                        u16 ctx_id = update_contexts_[update_depth_];

                        if (ctx_id & 0x8000) {
                            ctx_id &= 0x7FFF;
                            size_t idx = ctx_id;

                            if (free_block_head_ == NULL_PTR) {
                                reclaim_blocks();
                            }

                            u16 new_block = free_block_head_;
                            freq_next_[block] = new_block;
                            free_block_head_ = freq_next_[new_block];
                            freq_next_[new_block] = NULL_PTR;
                            freq_value_[new_block] = 1;
                            freq_char_[new_block] = character;
                            ctx_char_count_[idx]++;
                            ctx_low_freq_count_[idx]++;

                            update_context_stats(idx, new_block);
                        } else {
                            size_t idx = ctx_id;
                            freq_value_[block]++;

                            if (freq_value_[block] == LOW_FREQ_THRESHOLD) {
                                if (ctx_low_freq_count_[idx] > 0) ctx_low_freq_count_[idx]--;
                            }

                            update_context_stats(idx, block);
                        }
                    }
                }

                void update_context_stats(size_t idx, u16 block) {
                    ctx_total_freq_[idx]++;

                    u16 char_divisor = static_cast <u16>(ctx_char_count_[idx]) + 1;
                    if ((freq_value_[block] << 1) < ctx_total_freq_[idx] / char_divisor) {
                        if (ctx_rescale_factor_[idx] > 0) ctx_rescale_factor_[idx]--;
                    } else if (ctx_rescale_factor_[idx] < RESCALE_FACTOR_INIT) {
                        ctx_rescale_factor_[idx]++;
                    }

                    if (ctx_rescale_factor_[idx] == 0 || ctx_total_freq_[idx] >= MAX_TOTAL_FREQ) {
                        ctx_rescale_factor_[idx]++;
                        ctx_low_freq_count_[idx] = 0;
                        ctx_total_freq_[idx] = 0;

                        u16 blk = static_cast <u16>(idx);
                        while (blk != NULL_PTR) {
                            if (freq_value_[blk] > 1) {
                                freq_value_[blk] >>= 1;
                                ctx_total_freq_[idx] += freq_value_[blk];
                                if (freq_value_[blk] < LOW_FREQ_THRESHOLD) ctx_low_freq_count_[idx]++;
                            } else {
                                ctx_total_freq_[idx]++;
                                ctx_low_freq_count_[idx]++;
                            }
                            blk = freq_next_[blk];
                        }
                    }
                }

                arithmetic_decoder coder_;
                std::array <u8, 4> context_window_{};

                std::vector <u16> hash_heads_;
                std::vector <u16> hash_chain_;
                std::vector <u16> hash_rand_;

                std::vector <std::array <u8, 4>> ctx_bytes_;
                std::vector <u8> ctx_length_;
                std::vector <u8> ctx_char_count_;
                std::vector <u16> ctx_total_freq_;
                std::vector <u8> ctx_low_freq_count_;
                std::vector <u8> ctx_rescale_factor_;

                std::vector <u16> lru_prev_;
                std::vector <u16> lru_next_;
                u16 lru_front_ = 0;
                u16 lru_back_ = 0;

                std::vector <u16> freq_value_;
                std::vector <u8> freq_char_;
                std::vector <u16> freq_next_;
                u16 free_block_head_ = 0;
                u16 reclaim_cursor_ = 0;

                std::array <bool, 256> excluded_{};
                std::vector <u8> excluded_stack_;

                u8 non_escape_count_ = 0;
                std::array <u8, MAX_ORDER + 1> initial_escape_{};

                size_t update_depth_ = 0;
                std::array <u16, MAX_ORDER + 1> update_contexts_{};
                std::array <u16, MAX_ORDER + 1> update_blocks_{};

                std::array <u16, MAX_ORDER + 1> order_hashes_{};
                i16 search_order_ = 0;

                i16 order_reduction_counter_ = 0;
                u8 current_max_order_ = 0;
        };

        inline result_t <byte_vector> decompress_hsc(byte_span data) {
            hsc_decoder decoder(data);
            return decoder.decompress();
        }

        constexpr u8 MAGIC[2] = {'H', 'A'};

        enum Method : u8 {
            CPY = 0, // Stored (copy)
            ASC = 1, // LZ77 + Arithmetic coding
            HSC = 2, // PPM + Arithmetic coding
            DIR = 0x0E, // Directory entry
            SPECIAL = 0x0F // Special entry (symlink, device, etc.)
        };

        struct FileHeader {
            u8 version = 0;
            u8 method = 0;
            u32 compressed_size = 0;
            u32 original_size = 0;
            u32 crc32 = 0;
            dos_date_time datetime{};
            std::string path;
            std::string name;

            [[nodiscard]] std::string full_path() const {
                if (path.empty()) return name;
                return path + "/" + name;
            }

            [[nodiscard]] bool is_directory() const {
                return method == DIR;
            }
        };
    }

    struct ha_archive::impl {
        byte_vector data_;
        std::vector <ha::FileHeader> members_;
        std::vector <file_entry> files_;
    };

    ha_archive::ha_archive()
        : m_pimpl(std::make_unique <impl>()) {}

    ha_archive::~ha_archive() = default;

    result_t <std::unique_ptr <ha_archive>> ha_archive::open(byte_span data) {
        auto archive = std::unique_ptr <ha_archive>(new ha_archive());
        archive->m_pimpl->data_.assign(data.begin(), data.end());

        auto result = archive->parse();
        if (!result) return std::unexpected(result.error());

        return archive;
    }

    result_t <std::unique_ptr <ha_archive>> ha_archive::open(const std::filesystem::path& path) {
        auto file = file_input_stream::open(path);
        if (!file) return std::unexpected(file.error());

        auto size = file->size();
        if (!size) return std::unexpected(size.error());

        byte_vector data(*size);
        auto read = file->read(data);
        if (!read) return std::unexpected(read.error());

        return open(data);
    }

    const std::vector <file_entry>& ha_archive::files() const { return m_pimpl->files_; }

    result_t <byte_vector> ha_archive::extract(const file_entry& entry) {
        if (entry.folder_index >= m_pimpl->members_.size()) {
            return std::unexpected(error{error_code::FileNotInArchive});
        }

        const auto& member = m_pimpl->members_[entry.folder_index];
        if (entry.folder_offset + member.compressed_size > m_pimpl->data_.size()) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        byte_span compressed(m_pimpl->data_.data() + entry.folder_offset, member.compressed_size);
        byte_vector output;

        switch (member.method) {
            case ha::CPY:
                output.assign(compressed.begin(), compressed.end());
                break;

            case ha::DIR:
                // Directory entry, no data
                return byte_vector{};

            case ha::ASC: {
                ha_asc_decompressor decompressor;
                output.resize(member.original_size);
                auto result = decompressor.decompress_some(
                    compressed,
                    mutable_byte_span(output.data(), output.size()),
                    true
                );
                if (!result) return std::unexpected(result.error());
                if (result->bytes_written != member.original_size) {
                    return std::unexpected(error{error_code::CorruptData, "Incomplete decompression"});
                }
                break;
            }

            case ha::HSC: {
                ha_hsc_decompressor decompressor;
                output.resize(member.original_size);
                auto result = decompressor.decompress_some(
                    compressed,
                    mutable_byte_span(output.data(), output.size()),
                    true
                );
                if (!result) return std::unexpected(result.error());
                if (result->bytes_written != member.original_size) {
                    return std::unexpected(error{error_code::CorruptData, "Incomplete decompression"});
                }
                break;
            }

            case ha::SPECIAL:
                // Special entry (symlink, etc.), skip
                return byte_vector{};

            default:
                return std::unexpected(error{
                    error_code::UnsupportedCompression,
                    "Unsupported HA compression method"
                });
        }

        // Verify CRC-32
        u32 calc_crc = eval_crc_32(output);
        if (calc_crc != member.crc32) {
            return std::unexpected(error{error_code::InvalidChecksum, "CRC-32 mismatch"});
        }

        // Report byte-level progress
        if (byte_progress_cb_) {
            byte_progress_cb_(entry, output.size(), output.size());
        }

        return output;
    }

    void_result_t ha_archive::parse() {
        if (m_pimpl->data_.size() < 4) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        // Check magic
        if (m_pimpl->data_[0] != ha::MAGIC[0] || m_pimpl->data_[1] != ha::MAGIC[1]) {
            return std::unexpected(error{error_code::InvalidSignature, "Not a HA archive"});
        }

        // Read file count
        u16 file_count = read_u16_le(m_pimpl->data_.data() + 2);

        size_t pos = 4;

        for (u16 i = 0; i < file_count && pos < m_pimpl->data_.size(); i++) {
            if (pos >= m_pimpl->data_.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }

            // Read version/method byte
            u8 ver_type = m_pimpl->data_[pos++];
            u8 version = ver_type >> 4;
            u8 method = ver_type & 0x0F;

            if (pos + 16 > m_pimpl->data_.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }

            ha::FileHeader member;
            member.version = version;
            member.method = method;

            member.compressed_size = read_u32_le(m_pimpl->data_.data() + pos);
            pos += 4;
            member.original_size = read_u32_le(m_pimpl->data_.data() + pos);
            pos += 4;
            member.crc32 = read_u32_le(m_pimpl->data_.data() + pos);
            pos += 4;

            u32 timestamp = read_u32_le(m_pimpl->data_.data() + pos);
            member.datetime.date = static_cast <u16>(timestamp & 0xFFFF);
            member.datetime.time = static_cast <u16>((timestamp >> 16) & 0xFFFF);
            pos += 4;

            // Read path (null-terminated)
            while (pos < m_pimpl->data_.size() && m_pimpl->data_[pos] != 0) {
                member.path += static_cast <char>(m_pimpl->data_[pos++]);
            }
            if (pos < m_pimpl->data_.size()) pos++; // Skip null

            // Read name (null-terminated)
            while (pos < m_pimpl->data_.size() && m_pimpl->data_[pos] != 0) {
                member.name += static_cast <char>(m_pimpl->data_[pos++]);
            }
            if (pos < m_pimpl->data_.size()) pos++; // Skip null

            // Read machine info length and skip it
            if (pos >= m_pimpl->data_.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }
            u8 machine_info_len = m_pimpl->data_[pos++];
            pos += machine_info_len;

            // Store data offset
            size_t data_offset = pos;

            if (!member.is_directory() && method != ha::SPECIAL) {
                file_entry entry;
                entry.name = member.full_path();
                entry.uncompressed_size = member.original_size;
                entry.compressed_size = member.compressed_size;
                entry.datetime = member.datetime;
                entry.folder_index = static_cast <u32>(m_pimpl->members_.size());
                entry.folder_offset = data_offset;

                m_pimpl->files_.push_back(entry);
            }

            m_pimpl->members_.push_back(member);
            pos += member.compressed_size;
        }

        return {};
    }
} // namespace crate
