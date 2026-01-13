#pragma once

// RAR PPMd Decoder - Modern C++ port with simplified allocator
// Based on PPMd by Dmitry Shkarin (public domain, 1997-2000)

#include <crate/core/types.hh>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>
#include <stdexcept>

namespace crate::rar::ppm {
    inline u8 clamp_u8(unsigned value) {
        return value > std::numeric_limits<u8>::max()
            ? std::numeric_limits<u8>::max()
            : static_cast<u8>(value);
    }

    inline u16 clamp_u16(unsigned value) {
        return value > std::numeric_limits<u16>::max()
            ? std::numeric_limits<u16>::max()
            : static_cast<u16>(value);
    }
    // Constants
    constexpr int MAX_O = 64; // Maximum model order
    constexpr u32 TOP = 1u << 24;
    constexpr u32 BOT = 1u << 15;
    constexpr int INT_BITS = 7;
    constexpr int PERIOD_BITS = 7;
    constexpr int TOT_BITS = INT_BITS + PERIOD_BITS;
    constexpr int INTERVAL = 1 << INT_BITS;
    constexpr int BIN_SCALE = 1 << TOT_BITS;
    constexpr int MAX_FREQ = 124;
    // Note: Original PPMd used UNIT_SIZE=12 assuming 32-bit pointers.
    // On 64-bit systems, sizeof(State) = 16 due to pointer alignment.

    // Forward declarations
    class CRATE_EXPORT model_ppm;

    // PPM context node (forward declare for State)
    struct CRATE_EXPORT context;

    // PPM state (symbol + frequency + successor pointer)
    // Must be defined before SubAllocator which uses sizeof(State)
    struct CRATE_EXPORT state {
        u8 symbol = 0;
        u8 freq = 0;
        context* successor = nullptr;
    };

    // Simplified SubAllocator using new/delete
    // The original used a complex memory pool; modern allocators are fast enough
    class CRATE_EXPORT sub_allocator {
        public:
            sub_allocator() = default;
            ~sub_allocator();

            void start(int size_mb);

            void stop();

            void init();

            // Linear text area (for storing symbol history)
            u8* text();
            u8* text_start();

            // Check if we've used too much text space (7/8 of heap)
            u8* fake_units_start();

            u8* heap_end();

            void advance_text(size_t n = 1);
            void retreat_text(size_t n = 1);

            // Allocate a Context object (size determined at runtime since Context is forward-declared)
            void* alloc_context(); // Implementation after Context is defined

            // Allocate space for 'count' State objects
            void* alloc_units(int count);

            // Free units (simplified - we track but don't actually reuse)
            static void free_units(void* ptr, int /*count*/);

            // Expand allocation by one unit
            void* expand_units(void* old_ptr, int old_count);

            // Shrink allocation (no-op in simplified version)
            static void* shrink_units(void* ptr, int /*old_count*/, int /*new_count*/);

            [[nodiscard]] long allocated_memory() const;

        private:
            std::vector <u8> heap_;
            size_t heap_size_ = 0;
            size_t text_pos_ = 0;
            std::vector <void*> allocated_; // Track allocations for cleanup
    };

    // SEE2 context for PPM contexts with masked symbols
    struct CRATE_EXPORT see2_context {
        u16 summ = 0;
        u8 shift = 0;
        u8 count = 0;

        void init(int init_val);

        unsigned get_mean();

        void update();
    };

    // PPM context node
    struct CRATE_EXPORT context {
        u16 num_stats = 0;

        struct CRATE_EXPORT FreqData {
            u16 summ_freq = 0;
            state* stats = nullptr;
        };

        union {
            FreqData u;
            state one_state;
        };

        context* suffix = nullptr;

        // Decode methods (implemented in ModelPPM)
        void decode_bin_symbol(model_ppm* model);
        bool decode_symbol1(model_ppm* model);
        bool decode_symbol2(model_ppm* model);
        void update1(model_ppm* model, state* p);
        void update2(model_ppm* model, state* p);
        void rescale(model_ppm* model);
        context* create_child(model_ppm* model, state* pstats, state& first_state);
        see2_context* make_esc_freq2(model_ppm* model, int diff) const;
    };

    // Deferred implementation of SubAllocator::alloc_context (needs sizeof(Context))
    inline void* sub_allocator::alloc_context() {
        void* p = ::operator new(sizeof(context), std::nothrow);
        if (p) {
            allocated_.push_back(p);
            std::memset(p, 0, sizeof(context));
        }
        return p;
    }

    // Input adapter - abstracts byte reading
    class CRATE_EXPORT input_adapter {
        public:
            virtual ~input_adapter() = default;
            virtual u8 get_byte() = 0;
    };

    // Span-based input adapter (for byte-aligned input)
    class CRATE_EXPORT span_input : public input_adapter {
        public:
            span_input(byte_span data)
                : data_(data), pos_(0) {
            }

            u8 get_byte() override {
                if (pos_ < data_.size()) {
                    return data_[pos_++];
                }
                return 0;
            }

            void skip(size_t n) { pos_ += n; }
            [[nodiscard]] size_t position() const { return pos_; }

        private:
            byte_span data_;
            size_t pos_;
    };

    // Forward declaration for bit stream input
    class CRATE_EXPORT rar_bit_input_base;

    // Bit-stream input adapter (reads 8 bits at a time from bit stream)
    // Used for PPM which requires byte-aligned reads from a non-aligned position
    class CRATE_EXPORT bit_stream_input : public input_adapter {
        public:
            bit_stream_input(byte_span data, size_t byte_pos, unsigned bit_pos)
                : data_(data), byte_pos_(byte_pos), bit_pos_(bit_pos) {
            }

            // This matches unrar's GetChar() behavior:
            // 1. Advance 8 bits
            // 2. Return the byte at position BEFORE the new position
            // This effectively reads whole bytes, NOT bit-extracted bytes
            u8 get_byte() override {
                // Save current byte position BEFORE advancing
                size_t result_pos = byte_pos_;

                // Advance by 8 bits (like unrar's AddBits(8))
                bit_pos_ += 8;
                byte_pos_ += bit_pos_ >> 3;
                bit_pos_ &= 7;

                // Return the byte at the ORIGINAL position (like unrar's InBuf[InAddr-1])
                if (result_pos >= data_.size()) {
                    return 0;
                }

                return data_[result_pos];
            }

            [[nodiscard]] size_t byte_position() const { return byte_pos_; }
            [[nodiscard]] unsigned bit_position() const { return bit_pos_; }

        private:
            byte_span data_;
            size_t byte_pos_;
            unsigned bit_pos_;
    };

    // Range coder (arithmetic decoder)
    class CRATE_EXPORT range_coder {
        public:
            struct CRATE_EXPORT SubRange {
                u32 low_count = 0;
                u32 high_count = 0;
                u32 scale = 0;
            };

            void init(input_adapter* input) {
                input_ = input;
                low_ = code_ = 0;
                range_ = 0xFFFFFFFF;
                for (int i = 0; i < 4; i++) {
                    code_ = (code_ << 8) | input_->get_byte();
                }
            }

            int get_current_count() {
                range_ /= sub_range.scale;
                return static_cast <int>((code_ - low_) / range_);
            }

            unsigned get_current_shift_count(unsigned shift) {
                range_ >>= shift;
                return (code_ - low_) / range_;
            }

            void decode() {
                low_ += range_ * sub_range.low_count;
                range_ *= sub_range.high_count - sub_range.low_count;
            }

            void normalize() {
                while ((low_ ^ (low_ + range_)) < TOP ||
                       (range_ < BOT && ((range_ = (0u - low_) & (BOT - 1)), true))) {
                    code_ = (code_ << 8) | input_->get_byte();
                    range_ <<= 8;
                    low_ <<= 8;
                }
            }

            u8 get_byte() { return input_->get_byte(); }

            SubRange sub_range;

        private:
            input_adapter* input_ = nullptr;
            u32 low_ = 0;
            u32 code_ = 0;
            u32 range_ = 0;
    };

    // Main PPM model
    class CRATE_EXPORT model_ppm {
        friend struct context;

        public:
            model_ppm() = default;

            // Initialize decoder from input stream
            // Returns escape character via esc_char parameter
            bool decode_init(input_adapter* input, int& esc_char) {
                int max_order = input->get_byte();
                bool reset = (max_order & 0x20) != 0;

                int max_mb = 0;
                if (reset) {
                    max_mb = input->get_byte();
                } else if (sub_alloc_.allocated_memory() == 0) {
                    return false;
                }

                if (max_order & 0x40) {
                    esc_char = input->get_byte();
                }

                coder_.init(input);

                if (reset) {
                    max_order = (max_order & 0x1F) + 1;
                    if (max_order > 16) {
                        max_order = 16 + (max_order - 16) * 3;
                    }
                    if (max_order == 1) {
                        sub_alloc_.stop();
                        return false;
                    }
                    sub_alloc_.start(max_mb + 1);
                    if (!start_model_rare(max_order)) {
                        return false;
                    }
                }

                return min_context_ != nullptr;
            }

            // Decode single character
            int decode_char() {
                // Note: Original PPMd had heap range checks here, but we use separate allocations
                // so those checks don't apply. We just check for nullptr.
                if (!min_context_) {
                    return -1;
                }

                if (min_context_->num_stats != 1) {
                    if (!min_context_->u.stats) {
                        return -1;
                    }
                    if (!min_context_->decode_symbol1(this)) {
                        return -1;
                    }
                } else {
                    min_context_->decode_bin_symbol(this);
                }

                coder_.decode();

                while (!found_state_) {
                    coder_.normalize();
                    do {
                        order_fall_++;
                        min_context_ = min_context_->suffix;
                        if (!min_context_) {
                            return -1;
                        }
                    }
                    while (min_context_->num_stats == num_masked_);

                    if (!min_context_->decode_symbol2(this)) {
                        return -1;
                    }
                    coder_.decode();
                }

                int symbol = found_state_->symbol;

                // Note: Original PPMd checked if successor > text (within heap).
                // With separate allocations, we just check if successor is valid.
                if (!order_fall_ && found_state_->successor) {
                    min_context_ = max_context_ = found_state_->successor;
                } else {
                    update_model();
                    if (esc_count_ == 0) {
                        clear_mask();
                    }
                }

                coder_.normalize();
                return symbol;
            }

            void cleanup() {
                sub_alloc_.stop();
                sub_alloc_.start(1);
                start_model_rare(2);
            }

        private:
            bool restart_model_rare() {
                std::memset(char_mask_, 0, sizeof(char_mask_));
                sub_alloc_.init();

                init_rl_ = -(max_order_ < 12 ? max_order_ : 12) - 1;
                min_context_ = max_context_ = static_cast <context*>(sub_alloc_.alloc_context());
                if (!min_context_) {
                    min_context_ = nullptr;
                    max_context_ = nullptr;
                    found_state_ = nullptr;
                    return false;
                }

                min_context_->suffix = nullptr;
                order_fall_ = max_order_;
                min_context_->u.summ_freq = (min_context_->num_stats = 256) + 1;
                found_state_ = min_context_->u.stats = static_cast <state*>(sub_alloc_.alloc_units(256));
                if (!found_state_) {
                    min_context_ = nullptr;
                    max_context_ = nullptr;
                    found_state_ = nullptr;
                    return false;
                }

                run_length_ = init_rl_;
                prev_success_ = 0;
                for (int i = 0; i < 256; i++) {
                    min_context_->u.stats[i].symbol = static_cast <u8>(i);
                    min_context_->u.stats[i].freq = 1;
                    min_context_->u.stats[i].successor = nullptr;
                }

                static const u16 init_bin_esc[] = {
                    0x3CDD, 0x1F3F, 0x59BF, 0x48F3, 0x64A1, 0x5ABC, 0x6632, 0x6051
                };

                for (int i = 0; i < 128; i++) {
                    for (int k = 0; k < 8; k++) {
                        for (int m = 0; m < 64; m += 8) {
                            bin_summ_[i][k + m] = static_cast <u16>(BIN_SCALE - init_bin_esc[k] / (i + 2));
                        }
                    }
                }

                for (int i = 0; i < 25; i++) {
                    for (int k = 0; k < 16; k++) {
                        see2_cont_[i][k].init(5 * i + 10);
                    }
                }
                return true;
            }

            bool start_model_rare(int max_order) {
                esc_count_ = 1;
                max_order_ = max_order;
                if (!restart_model_rare()) {
                    return false;
                }

                ns2bs_indx_[0] = 2 * 0;
                ns2bs_indx_[1] = 2 * 1;
                std::memset(ns2bs_indx_ + 2, 2 * 2, 9);
                std::memset(ns2bs_indx_ + 11, 2 * 3, 256 - 11);

                for (int i = 0; i < 3; i++) {
                    ns2_indx_[i] = static_cast <u8>(i);
                }

                for (int m = 3, k = 1, step = 1, i = 3; i < 256; i++) {
                    ns2_indx_[i] = static_cast <u8>(m);
                    if (--k == 0) {
                        k = ++step;
                        m++;
                    }
                }

                std::memset(hb2_flag_, 0, 0x40);
                std::memset(hb2_flag_ + 0x40, 0x08, 0x100 - 0x40);
                dummy_see2_cont_.shift = PERIOD_BITS;
                return true;
            }

            context* create_successors(bool skip, state* p1);
            void update_model();

            void clear_mask() {
                esc_count_ = 1;
                std::memset(char_mask_, 0, sizeof(char_mask_));
            }

            // Member variables
            see2_context see2_cont_[25][16];
            see2_context dummy_see2_cont_;

            context* min_context_ = nullptr;
            context* med_context_ = nullptr;
            context* max_context_ = nullptr;
            state* found_state_ = nullptr;

            int num_masked_ = 0;
            int init_esc_ = 0;
            int order_fall_ = 0;
            int max_order_ = 0;
            int run_length_ = 0;
            int init_rl_ = 0;

            u8 char_mask_[256] = {};
            u8 ns2_indx_[256] = {};
            u8 ns2bs_indx_[256] = {};
            u8 hb2_flag_[256] = {};

            u8 esc_count_ = 0;
            u8 prev_success_ = 0;
            u8 hi_bits_flag_ = 0;

            u16 bin_summ_[128][64] = {};

            range_coder coder_;
            sub_allocator sub_alloc_;
    };

    // Tabulated escapes for exponential symbol distribution
    inline constexpr u8 exp_escape[16] = {25, 14, 9, 7, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 2};

    // Inline implementations of Context methods

    inline context* context::create_child(model_ppm* model, state* pstats, state& first_state) {
        auto* pc = static_cast <context*>(model->sub_alloc_.alloc_context());
        if (pc) {
            pc->num_stats = 1;
            pc->one_state = first_state;
            pc->suffix = this;
            pstats->successor = pc;
        }
        return pc;
    }

    inline void context::decode_bin_symbol(model_ppm* model) {
        state& rs = one_state;
        model->hi_bits_flag_ = model->hb2_flag_[model->found_state_->symbol];

        u16& bs = model->bin_summ_[rs.freq - 1][model->prev_success_ +
                                                model->ns2bs_indx_[suffix->num_stats - 1] +
                                                model->hi_bits_flag_ + 2 * model->hb2_flag_[rs.symbol] +
                                                ((model->run_length_ >> 26) & 0x20)];

        if (model->coder_.get_current_shift_count(TOT_BITS) < bs) {
            model->found_state_ = &rs;
            rs.freq = static_cast <u8>(rs.freq + static_cast <u8>(rs.freq < 128));
            model->coder_.sub_range.low_count = 0;
            model->coder_.sub_range.high_count = bs;
            bs = static_cast <u16>(bs + INTERVAL - ((bs + (1 << (PERIOD_BITS - 2))) >> PERIOD_BITS));
            model->prev_success_ = 1;
            model->run_length_++;
        } else {
            model->coder_.sub_range.low_count = bs;
            bs = static_cast <u16>(bs - ((bs + (1 << (PERIOD_BITS - 2))) >> PERIOD_BITS));
            model->coder_.sub_range.high_count = BIN_SCALE;
            model->init_esc_ = exp_escape[bs >> 10];
            model->num_masked_ = 1;
            model->char_mask_[rs.symbol] = model->esc_count_;
            model->prev_success_ = 0;
            model->found_state_ = nullptr;
        }
    }

    inline void context::update1(model_ppm* model, state* p) {
        (model->found_state_ = p)->freq += 4;
        u.summ_freq += 4;
        if (p[0].freq > p[-1].freq) {
            std::swap(p[0], p[-1]);
            model->found_state_ = --p;
            if (p->freq > MAX_FREQ) {
                rescale(model);
            }
        }
    }

    inline bool context::decode_symbol1(model_ppm* model) {
        model->coder_.sub_range.scale = u.summ_freq;
        state* p = u.stats;
        int hi_cnt;
        int count = model->coder_.get_current_count();

        if (count >= static_cast <int>(model->coder_.sub_range.scale)) {
            return false;
        }

        if (count < (hi_cnt = p->freq)) {
            u32 hi_cnt_u32 = static_cast <u32>(hi_cnt);
            model->coder_.sub_range.high_count = hi_cnt_u32;
            model->prev_success_ = (2 * hi_cnt_u32 > model->coder_.sub_range.scale);
            model->run_length_ += model->prev_success_;
            hi_cnt += 4;
            (model->found_state_ = p)->freq = clamp_u8(static_cast<unsigned>(hi_cnt));
            u.summ_freq += 4;
            if (hi_cnt > MAX_FREQ) {
                rescale(model);
            }
            model->coder_.sub_range.low_count = 0;
            return true;
        } else if (model->found_state_ == nullptr) {
            return false;
        }

        model->prev_success_ = 0;
        int i = num_stats - 1;

        while ((hi_cnt += (++p)->freq) <= count) {
            if (--i == 0) {
                model->hi_bits_flag_ = model->hb2_flag_[model->found_state_->symbol];
                model->coder_.sub_range.low_count = static_cast <u32>(hi_cnt);
                model->char_mask_[p->symbol] = model->esc_count_;
                i = (model->num_masked_ = num_stats) - 1;
                model->found_state_ = nullptr;
                do {
                    model->char_mask_[(--p)->symbol] = model->esc_count_;
                }
                while (--i);
                model->coder_.sub_range.high_count = model->coder_.sub_range.scale;
                return true;
            }
        }

        u32 hi_cnt_u32 = static_cast <u32>(hi_cnt);
        model->coder_.sub_range.high_count = hi_cnt_u32;
        model->coder_.sub_range.low_count = hi_cnt_u32 - p->freq;
        update1(model, p);
        return true;
    }

    inline void context::update2(model_ppm* model, state* p) {
        (model->found_state_ = p)->freq = clamp_u8(static_cast<unsigned>(p->freq + 4));
        u.summ_freq += 4;
        if (p->freq > MAX_FREQ) {
            rescale(model);
        }
        model->esc_count_++;
        model->run_length_ = model->init_rl_;
    }

    inline see2_context* context::make_esc_freq2(model_ppm* model, int diff) const {
        see2_context* psee2c;
        if (num_stats != 256) {
            psee2c = &model->see2_cont_[model->ns2_indx_[diff - 1]]
            [(diff < suffix->num_stats - num_stats) +
             2 * (u.summ_freq < 11 * num_stats) +
             4 * (model->num_masked_ > diff) +
             model->hi_bits_flag_];
            model->coder_.sub_range.scale = psee2c->get_mean();
        } else {
            psee2c = &model->dummy_see2_cont_;
            model->coder_.sub_range.scale = 1;
        }
        return psee2c;
    }

    inline bool context::decode_symbol2(model_ppm* model) {
        int count, hi_cnt, i = num_stats - model->num_masked_;
        see2_context* psee2c = make_esc_freq2(model, i);
        state* ps[256];
        state** pps = ps;
        state* p = u.stats - 1;

        hi_cnt = 0;
        do {
            do {
                p++;
            }
            while (model->char_mask_[p->symbol] == model->esc_count_);
            hi_cnt += p->freq;
            if (pps >= ps + 256) {
                return false;
            }
            *pps++ = p;
        }
        while (--i);

        model->coder_.sub_range.scale += static_cast <u32>(hi_cnt);
        count = model->coder_.get_current_count();

        if (count >= static_cast <int>(model->coder_.sub_range.scale)) {
            return false;
        }

        p = *(pps = ps);
        if (count < hi_cnt) {
            hi_cnt = 0;
            while ((hi_cnt += p->freq) <= count) {
                pps++;
                if (pps >= ps + 256) {
                    return false;
                }
                p = *pps;
            }
            u32 hi_cnt_u32 = static_cast <u32>(hi_cnt);
            model->coder_.sub_range.high_count = hi_cnt_u32;
            model->coder_.sub_range.low_count = hi_cnt_u32 - p->freq;
            psee2c->update();
            update2(model, p);
        } else {
            model->coder_.sub_range.low_count = static_cast <u32>(hi_cnt);
            model->coder_.sub_range.high_count = model->coder_.sub_range.scale;
            i = num_stats - model->num_masked_;

            do {
                if (pps >= ps + 256) {
                    return false;
                }
                model->char_mask_[(*pps)->symbol] = model->esc_count_;
                pps++;
            }
            while (--i);

            psee2c->summ += static_cast <u16>(model->coder_.sub_range.scale);
            model->num_masked_ = num_stats;
        }
        return true;
    }

    inline void context::rescale(model_ppm* model) {
        int old_ns = num_stats, i = num_stats - 1, adder, esc_freq;
        state* p1;
        state* p;

        for (p = model->found_state_; p != u.stats; p--) {
            std::swap(p[0], p[-1]);
        }

        u.stats->freq += 4;
        u.summ_freq += 4;
        esc_freq = u.summ_freq - p->freq;
        adder = (model->order_fall_ != 0);
        u.summ_freq = (p->freq = clamp_u8(static_cast<unsigned>((p->freq + adder) >> 1)));

        do {
            esc_freq -= (++p)->freq;
            u.summ_freq += (p->freq = clamp_u8(static_cast<unsigned>((p->freq + adder) >> 1)));
            if (p[0].freq > p[-1].freq) {
                state tmp = *(p1 = p);
                do {
                    p1[0] = p1[-1];
                }
                while (--p1 != u.stats && tmp.freq > p1[-1].freq);
                *p1 = tmp;
            }
        }
        while (--i);

        if (p->freq == 0) {
            do {
                i++;
            }
            while ((--p)->freq == 0);

            esc_freq += i;
            num_stats = clamp_u16(static_cast<unsigned>(num_stats - static_cast <u16>(i)));
            if (num_stats == 1) {
                state tmp = *u.stats;
                do {
                    tmp.freq = clamp_u8(static_cast<unsigned>(tmp.freq - (tmp.freq >> 1)));
                    esc_freq >>= 1;
                }
                while (esc_freq > 1);
                sub_allocator::free_units(u.stats, old_ns);
                *(model->found_state_ = &one_state) = tmp;
                return;
            }
        }

        esc_freq -= (esc_freq >> 1);
        u.summ_freq = clamp_u16(static_cast<unsigned>(u.summ_freq + static_cast <u16>(esc_freq)));
        // Shrink allocation if significantly smaller (shrink_units is currently a no-op)
        if (num_stats < old_ns) {
            u.stats = static_cast <state*>(model->sub_alloc_.shrink_units(u.stats, old_ns, num_stats));
        }
        model->found_state_ = u.stats;
    }

    inline context* model_ppm::create_successors(bool skip, state* p1) {
        state up_state;
        context* pc = min_context_;
        context* up_branch = found_state_->successor;
        state* p;
        state* ps[MAX_O];
        state** pps = ps;

        if (!skip) {
            *pps++ = found_state_;
            if (!pc->suffix) {
                goto NO_LOOP;
            }
        }

        if (p1) {
            p = p1;
            pc = pc->suffix;
            goto LOOP_ENTRY;
        }

        do {
            pc = pc->suffix;
            if (pc->num_stats != 1) {
                if ((p = pc->u.stats)->symbol != found_state_->symbol) {
                    do {
                        p++;
                    }
                    while (p->symbol != found_state_->symbol);
                }
            } else {
                p = &pc->one_state;
            }

        LOOP_ENTRY:
            if (p->successor != up_branch) {
                pc = p->successor;
                break;
            }

            if (pps >= ps + MAX_O) {
                return nullptr;
            }
            *pps++ = p;
        }
        while (pc->suffix);

    NO_LOOP:
        if (pps == ps) {
            return pc;
        }

        up_state.symbol = *reinterpret_cast <u8*>(up_branch);
        up_state.successor = reinterpret_cast <context*>(reinterpret_cast <u8*>(up_branch) + 1);

        if (pc->num_stats != 1) {
            // Note: Original PPMd had heap range check here, but with separate allocations
            // we just check for nullptr
            if (!pc) {
                return nullptr;
            }
            if ((p = pc->u.stats)->symbol != up_state.symbol) {
                do {
                    p++;
                }
                while (p->symbol != up_state.symbol);
            }
            unsigned cf = p->freq - 1;
            unsigned s0 = pc->u.summ_freq - pc->num_stats - cf;
            up_state.freq = clamp_u8(1u + ((2u * cf <= s0)
                                                  ? static_cast <unsigned>(5u * cf > s0)
                                                  : (2u * cf + 3u * s0 - 1u) / (2u * s0)));
        } else {
            up_state.freq = pc->one_state.freq;
        }

        do {
            pc = pc->create_child(this, *--pps, up_state);
            if (!pc) {
                return nullptr;
            }
        }
        while (pps != ps);

        return pc;
    }

    inline void model_ppm::update_model() {
        state fs = *found_state_;
        state* p = nullptr;
        context* pc;
        context* successor;
        unsigned ns1, ns, cf, sf, s0;

        if (fs.freq < MAX_FREQ / 4 && (pc = min_context_->suffix) != nullptr) {
            if (pc->num_stats != 1) {
                if ((p = pc->u.stats)->symbol != fs.symbol) {
                    do {
                        p++;
                    }
                    while (p->symbol != fs.symbol);
                    if (p[0].freq >= p[-1].freq) {
                        std::swap(p[0], p[-1]);
                        p--;
                    }
                }
                if (p->freq < MAX_FREQ - 9) {
                    p->freq += 2;
                    pc->u.summ_freq += 2;
                }
            } else {
                p = &pc->one_state;
                p->freq = clamp_u8(p->freq + static_cast<unsigned>(p->freq < 32));
            }
        }

        if (!order_fall_) {
            min_context_ = max_context_ = found_state_->successor = create_successors(true, p);
            if (!min_context_) {
                goto RESTART_MODEL;
            }
            return;
        }

        *sub_alloc_.text() = fs.symbol;
        sub_alloc_.advance_text();
        successor = reinterpret_cast <context*>(sub_alloc_.text());

        if (sub_alloc_.text() >= sub_alloc_.fake_units_start()) {
            goto RESTART_MODEL;
        }

        if (fs.successor) {
            if (reinterpret_cast <u8*>(fs.successor) <= sub_alloc_.text() &&
                (fs.successor = create_successors(false, p)) == nullptr) {
                goto RESTART_MODEL;
            }
            if (!--order_fall_) {
                successor = fs.successor;
                sub_alloc_.retreat_text(max_context_ != min_context_ ? 1 : 0);
            }
        } else {
            found_state_->successor = successor;
            fs.successor = min_context_;
        }

        s0 = min_context_->u.summ_freq - (ns = min_context_->num_stats) - (fs.freq - 1);

        for (pc = max_context_; pc != min_context_; pc = pc->suffix) {
            if ((ns1 = pc->num_stats) != 1) {
                // Expand stats array to add one more State
                pc->u.stats = static_cast <state*>(sub_alloc_.expand_units(pc->u.stats,
                                                                          static_cast <int>(ns1)));
                if (!pc->u.stats) {
                    goto RESTART_MODEL;
                }
                pc->u.summ_freq = clamp_u16(
                    pc->u.summ_freq +
                    static_cast <u16>((2u * ns1 < ns) +
                                      2u * ((4u * ns1 <= ns) &
                                            (pc->u.summ_freq <= 8u * ns1))));
            } else {
                // Allocate initial 2-element stats array
                p = static_cast <state*>(sub_alloc_.alloc_units(2));
                if (!p) {
                    goto RESTART_MODEL;
                }
                *p = pc->one_state;
                pc->u.stats = p;
                if (p->freq < MAX_FREQ / 4 - 1) {
                    p->freq += p->freq;
                } else {
                    p->freq = MAX_FREQ - 4;
                }
                pc->u.summ_freq = clamp_u16(static_cast<unsigned>(p->freq + init_esc_ +
                                                                 static_cast <u16>(ns > 3)));
            }

            cf = 2u * fs.freq * static_cast <unsigned>(pc->u.summ_freq + 6);
            sf = s0 + pc->u.summ_freq;

            if (cf < 6 * sf) {
                cf = 1u + static_cast <unsigned>(cf > sf) +
                     static_cast <unsigned>(cf >= 4u * sf);
                pc->u.summ_freq += 3;
            } else {
                cf = 4u + static_cast <unsigned>(cf >= 9u * sf) +
                     static_cast <unsigned>(cf >= 12u * sf) +
                     static_cast <unsigned>(cf >= 15u * sf);
                pc->u.summ_freq = clamp_u16(pc->u.summ_freq + static_cast <u16>(cf));
            }

            p = pc->u.stats + ns1;
            p->successor = successor;
            p->symbol = fs.symbol;
            p->freq = clamp_u8(cf);
            pc->num_stats = clamp_u16(static_cast<unsigned>(++ns1));
        }

        max_context_ = min_context_ = fs.successor;
        return;

    RESTART_MODEL:
        if (!restart_model_rare()) {
            esc_count_ = 0;
            min_context_ = nullptr;
            max_context_ = nullptr;
            found_state_ = nullptr;
            return;
        }
        esc_count_ = 0;
    }
} // namespace crate::rar::ppm
