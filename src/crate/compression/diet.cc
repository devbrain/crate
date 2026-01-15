#include <crate/compression/diet.hh>
#include <vector>

namespace crate {

namespace {
    constexpr size_t DLZ_HEADER_SIZE = 17;
    constexpr size_t HISTORY_SIZE = 65536;
}

struct diet_decompressor::impl {
    // State machine
    enum class state {
        READ_HEADER,
        PRIME_BIT_BUFFER,
        COPY_LITERALS_CHECK,
        COPY_LITERAL_BYTE,
        READ_MATCH_TYPE,
        READ_OFFSET_LO,

        // Extended offset path
        EXT_RCL_BIT1,
        EXT_CHECK_BIT,
        EXT_LOOP_CHECK,
        EXT_LOOP_RCL,

        // Length decoding
        LEN_LOOP_BIT,
        LEN_EXT_CHECK,
        LEN_TYPE_BIT,
        LEN_READ_BYTE,
        LEN_3BIT_LOOP,
        LEN_FINAL_BIT,

        // Short offset path
        SHORT_CHECK_BIT,
        SHORT_3BIT_LOOP,
        SHORT_TERM_BIT,

        // Copy match
        COPY_MATCH,

        DONE
    };

    state state_ = state::READ_HEADER;

    // Header parsing
    std::array<u8, DLZ_HEADER_SIZE> header_buf_{};
    size_t header_pos_ = 0;
    size_t compressed_size_ = 0;
    size_t decompressed_size_ = 0;

    // Bit buffer (matches original: start with 1 bit from code_word=0)
    u16 code_word_ = 0;
    u8 bits_remaining_ = 1;  // Start at 1 to match original behavior

    // Match decoding state
    u8 offset_lo_ = 0;
    u8 offset_hi_ = 0;
    u16 match_length_ = 0;
    u16 match_remaining_ = 0;
    u8 adjustment_ = 0;
    u16 counter_ = 0;
    u8 bit_idx_ = 0;  // For 3-bit loops
    bool extended_path_ = false;  // Track which path we're on

    // History buffer for streaming
    std::vector<u8> history_;
    size_t history_pos_ = 0;
    size_t total_output_ = 0;

    impl() : history_(HISTORY_SIZE, 0) {}

    void reset() {
        state_ = state::READ_HEADER;
        header_pos_ = 0;
        compressed_size_ = 0;
        decompressed_size_ = 0;
        code_word_ = 0;
        bits_remaining_ = 1;
        offset_lo_ = 0;
        offset_hi_ = 0;
        match_length_ = 0;
        match_remaining_ = 0;
        adjustment_ = 0;
        counter_ = 0;
        bit_idx_ = 0;
        extended_path_ = false;
        std::fill(history_.begin(), history_.end(), 0);
        history_pos_ = 0;
        total_output_ = 0;
    }

    // Rotate left through carry
    static void rcl(u8& value, bool carry_in) {
        value = static_cast<u8>((value << 1) | (carry_in ? 1 : 0));
    }

    // Try to get next bit. Returns -1 if need more input.
    // Key insight: reload must happen ATOMICALLY with bit use.
    // If using the last bit would require a reload, we must have 2 bytes available.
    int try_next_bit(const byte*& ptr, const byte* end) {
        // If bit buffer is empty, reload first
        if (bits_remaining_ == 0) {
            if (end - ptr < 2) {
                return -1;
            }
            u8 lo = static_cast<u8>(*ptr++);
            u8 hi = static_cast<u8>(*ptr++);
            code_word_ = static_cast<u16>((static_cast<unsigned>(hi) << 8) | lo);
            bits_remaining_ = 16;
        }

        // Check: if this is the last bit, we'll need to reload after using it.
        // We must have 2 bytes available for that reload, OR not need to reload yet.
        if (bits_remaining_ == 1 && end - ptr < 2) {
            // Can't use this bit because we won't be able to reload after
            return -1;
        }

        // Use the bit
        bool bit = (code_word_ & 1) != 0;
        code_word_ >>= 1;
        bits_remaining_--;

        // Reload if needed (we've verified above that we have enough bytes)
        if (bits_remaining_ == 0) {
            u8 lo = static_cast<u8>(*ptr++);
            u8 hi = static_cast<u8>(*ptr++);
            code_word_ = static_cast<u16>((static_cast<unsigned>(hi) << 8) | lo);
            bits_remaining_ = 16;
        }

        return bit ? 1 : 0;
    }

    // Try to read a byte. Returns -1 if need more input.
    int try_read_byte(const byte*& ptr, const byte* end) {
        if (ptr >= end) {
            return -1;
        }
        return static_cast<u8>(*ptr++);
    }

    // Write byte to output and history
    void write_byte(u8 value, byte*& out_ptr, byte* out_end) {
        if (out_ptr < out_end) {
            *out_ptr++ = value;
        }
        history_[history_pos_++ & (HISTORY_SIZE - 1)] = value;
        total_output_++;
    }

    // Get byte from history
    u8 get_history(size_t distance) const {
        return history_[(history_pos_ - distance) & (HISTORY_SIZE - 1)];
    }

    result_t<void> parse_header() {
        if (header_buf_[6] != 'd' || header_buf_[7] != 'l' || header_buf_[8] != 'z') {
            return std::unexpected(error{error_code::InvalidSignature,
                "Not a valid DLZ/DIET file"});
        }

        u8 flags = header_buf_[9];
        u16 size_lo = static_cast<u16>((static_cast<unsigned>(header_buf_[11]) << 8) |
                                        header_buf_[10]);
        compressed_size_ = (static_cast<size_t>(flags & 0x0F) << 16) | size_lo;

        u8 size_hi = header_buf_[14];
        u16 decomp_lo = static_cast<u16>((static_cast<unsigned>(header_buf_[16]) << 8) |
                                          header_buf_[15]);
        decompressed_size_ = (static_cast<size_t>((size_hi >> 2) & 0x3F) << 16) | decomp_lo;

        return {};
    }

    result_t<stream_result> decompress(
        byte_span input,
        mutable_byte_span output,
        bool input_finished,
        const std::function<void(size_t, size_t)>& progress_cb
    ) {
        const byte* in_ptr = input.data();
        const byte* in_end = input.data() + input.size();
        byte* out_ptr = output.data();
        byte* out_end = output.data() + output.size();

        int bit;
        int byte_val;

        while (state_ != state::DONE) {
            switch (state_) {
                case state::READ_HEADER:
                    while (header_pos_ < DLZ_HEADER_SIZE) {
                        if (in_ptr >= in_end) goto need_input;
                        header_buf_[header_pos_++] = static_cast<u8>(*in_ptr++);
                    }
                    {
                        auto result = parse_header();
                        if (!result) return std::unexpected(result.error());
                    }
                    state_ = state::PRIME_BIT_BUFFER;
                    break;

                case state::PRIME_BIT_BUFFER:
                    bit = try_next_bit(in_ptr, in_end);
                    if (bit < 0) goto need_input;
                    // Discard the priming bit
                    state_ = state::COPY_LITERALS_CHECK;
                    break;

                case state::COPY_LITERALS_CHECK:
                    bit = try_next_bit(in_ptr, in_end);
                    if (bit < 0) goto need_input;
                    if (bit) {
                        state_ = state::COPY_LITERAL_BYTE;
                    } else {
                        state_ = state::READ_MATCH_TYPE;
                    }
                    break;

                case state::COPY_LITERAL_BYTE:
                    if (out_ptr >= out_end) goto need_output;
                    byte_val = try_read_byte(in_ptr, in_end);
                    if (byte_val < 0) goto need_input;
                    write_byte(static_cast<u8>(byte_val), out_ptr, out_end);
                    state_ = state::COPY_LITERALS_CHECK;
                    break;

                case state::READ_MATCH_TYPE:
                    bit = try_next_bit(in_ptr, in_end);
                    if (bit < 0) goto need_input;
                    extended_path_ = (bit != 0);
                    offset_hi_ = 0xFF;
                    state_ = state::READ_OFFSET_LO;
                    break;

                case state::READ_OFFSET_LO:
                    byte_val = try_read_byte(in_ptr, in_end);
                    if (byte_val < 0) goto need_input;
                    offset_lo_ = static_cast<u8>(byte_val);
                    if (extended_path_) {
                        state_ = state::EXT_RCL_BIT1;
                    } else {
                        state_ = state::SHORT_CHECK_BIT;
                    }
                    break;

                // ===== Extended offset path =====
                case state::EXT_RCL_BIT1:
                    bit = try_next_bit(in_ptr, in_end);
                    if (bit < 0) goto need_input;
                    rcl(offset_hi_, bit);
                    state_ = state::EXT_CHECK_BIT;
                    break;

                case state::EXT_CHECK_BIT:
                    bit = try_next_bit(in_ptr, in_end);
                    if (bit < 0) goto need_input;
                    if (bit) {
                        // No further extension, go to length decoding
                        adjustment_ = 2;
                        counter_ = 4;
                        state_ = state::LEN_LOOP_BIT;
                    } else {
                        // Further extend offset
                        adjustment_ = 2;
                        counter_ = 3;
                        state_ = state::EXT_LOOP_CHECK;
                    }
                    break;

                case state::EXT_LOOP_CHECK:
                    bit = try_next_bit(in_ptr, in_end);
                    if (bit < 0) goto need_input;
                    if (bit) {
                        offset_hi_ -= adjustment_;
                        adjustment_ = 2;
                        counter_ = 4;
                        state_ = state::LEN_LOOP_BIT;
                    } else {
                        state_ = state::EXT_LOOP_RCL;
                    }
                    break;

                case state::EXT_LOOP_RCL:
                    bit = try_next_bit(in_ptr, in_end);
                    if (bit < 0) goto need_input;
                    rcl(offset_hi_, bit);
                    adjustment_ <<= 1;
                    counter_--;
                    if (counter_ == 0) {
                        offset_hi_ -= adjustment_;
                        adjustment_ = 2;
                        counter_ = 4;
                        state_ = state::LEN_LOOP_BIT;
                    } else {
                        state_ = state::EXT_LOOP_CHECK;
                    }
                    break;

                // ===== Length decoding =====
                case state::LEN_LOOP_BIT:
                    bit = try_next_bit(in_ptr, in_end);
                    if (bit < 0) goto need_input;
                    adjustment_++;  // Increment AFTER successful read
                    if (bit) {
                        match_length_ = adjustment_;
                        match_remaining_ = match_length_;
                        state_ = state::COPY_MATCH;
                    } else {
                        counter_--;
                        if (counter_ == 0) {
                            state_ = state::LEN_EXT_CHECK;
                        }
                        // else stay in LEN_LOOP_BIT
                    }
                    break;

                case state::LEN_EXT_CHECK:
                    bit = try_next_bit(in_ptr, in_end);
                    if (bit < 0) goto need_input;
                    if (bit) {
                        adjustment_++;
                        state_ = state::LEN_FINAL_BIT;
                    } else {
                        state_ = state::LEN_TYPE_BIT;
                    }
                    break;

                case state::LEN_TYPE_BIT:
                    bit = try_next_bit(in_ptr, in_end);
                    if (bit < 0) goto need_input;
                    if (bit) {
                        state_ = state::LEN_READ_BYTE;
                    } else {
                        adjustment_ = 0;
                        bit_idx_ = 0;
                        state_ = state::LEN_3BIT_LOOP;
                    }
                    break;

                case state::LEN_READ_BYTE:
                    byte_val = try_read_byte(in_ptr, in_end);
                    if (byte_val < 0) goto need_input;
                    match_length_ = static_cast<u16>(byte_val) + 17;
                    match_remaining_ = match_length_;
                    state_ = state::COPY_MATCH;
                    break;

                case state::LEN_3BIT_LOOP:
                    while (bit_idx_ < 3) {
                        bit = try_next_bit(in_ptr, in_end);
                        if (bit < 0) goto need_input;
                        rcl(adjustment_, bit);
                        bit_idx_++;
                    }
                    match_length_ = adjustment_ + 9;
                    match_remaining_ = match_length_;
                    state_ = state::COPY_MATCH;
                    break;

                case state::LEN_FINAL_BIT:
                    bit = try_next_bit(in_ptr, in_end);
                    if (bit < 0) goto need_input;
                    if (bit) {
                        adjustment_++;
                    }
                    match_length_ = adjustment_;
                    match_remaining_ = match_length_;
                    state_ = state::COPY_MATCH;
                    break;

                // ===== Short offset path =====
                case state::SHORT_CHECK_BIT:
                    bit = try_next_bit(in_ptr, in_end);
                    if (bit < 0) goto need_input;
                    if (bit) {
                        bit_idx_ = 0;
                        state_ = state::SHORT_3BIT_LOOP;
                    } else {
                        if (offset_lo_ == offset_hi_) {
                            state_ = state::SHORT_TERM_BIT;
                        } else {
                            match_length_ = 2;
                            match_remaining_ = 2;
                            state_ = state::COPY_MATCH;
                        }
                    }
                    break;

                case state::SHORT_3BIT_LOOP:
                    while (bit_idx_ < 3) {
                        bit = try_next_bit(in_ptr, in_end);
                        if (bit < 0) goto need_input;
                        rcl(offset_hi_, bit);
                        bit_idx_++;
                    }
                    offset_hi_--;
                    match_length_ = 2;
                    match_remaining_ = 2;
                    state_ = state::COPY_MATCH;
                    break;

                case state::SHORT_TERM_BIT:
                    bit = try_next_bit(in_ptr, in_end);
                    if (bit < 0) goto need_input;
                    if (bit) {
                        // Not termination, continue copying literals
                        state_ = state::COPY_LITERALS_CHECK;
                    } else {
                        // End of decompression
                        state_ = state::DONE;
                    }
                    break;

                // ===== Copy match =====
                case state::COPY_MATCH:
                    while (match_remaining_ > 0) {
                        if (out_ptr >= out_end) goto need_output;

                        i16 offset = static_cast<i16>(
                            (static_cast<u16>(offset_hi_) << 8) | offset_lo_);

                        if (offset >= 0) {
                            return std::unexpected(error{error_code::CorruptData,
                                "DLZ: invalid positive offset"});
                        }

                        size_t distance = static_cast<size_t>(-static_cast<i32>(offset));
                        if (distance > history_pos_) {
                            return std::unexpected(error{error_code::CorruptData,
                                "DLZ: offset exceeds history"});
                        }

                        u8 value = get_history(distance);
                        write_byte(value, out_ptr, out_end);
                        match_remaining_--;
                    }
                    state_ = state::COPY_LITERALS_CHECK;
                    break;

                case state::DONE:
                    break;
            }
        }

        // Successfully completed
        if (progress_cb) {
            progress_cb(total_output_, decompressed_size_);
        }
        return stream_result::done(
            static_cast<size_t>(in_ptr - input.data()),
            static_cast<size_t>(out_ptr - output.data())
        );

    need_output:
        return stream_result::need_output(
            static_cast<size_t>(in_ptr - input.data()),
            static_cast<size_t>(out_ptr - output.data())
        );

    need_input:
        if (input_finished) {
            return std::unexpected(error{error_code::TruncatedArchive,
                "Unexpected end of DIET data"});
        }
        return stream_result::need_input(
            static_cast<size_t>(in_ptr - input.data()),
            static_cast<size_t>(out_ptr - output.data())
        );
    }
};

diet_decompressor::diet_decompressor()
    : pimpl_(std::make_unique<impl>()) {
}

diet_decompressor::~diet_decompressor() = default;

result_t<stream_result> diet_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    return pimpl_->decompress(input, output, input_finished,
        [this](size_t written, size_t total) {
            report_progress(written, total);
        });
}

void diet_decompressor::reset() {
    pimpl_->reset();
}

} // namespace crate
