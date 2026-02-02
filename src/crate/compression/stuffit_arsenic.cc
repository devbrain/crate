#include <crate/compression/stuffit_arsenic.hh>
#include <algorithm>
#include <cstring>

namespace crate {

// Randomization table for Arsenic
const u16 stuffit_arsenic_decompressor::RandomizationTable[256] = {
    0xee, 0x56, 0xf8, 0xc3, 0x9d, 0x9f, 0xae, 0x2c,
    0xad, 0xcd, 0x24, 0x9d, 0xa6, 0x101, 0x18, 0xb9,
    0xa1, 0x82, 0x75, 0xe9, 0x9f, 0x55, 0x66, 0x6a,
    0x86, 0x71, 0xdc, 0x84, 0x56, 0x96, 0x56, 0xa1,
    0x84, 0x78, 0xb7, 0x32, 0x6a, 0x3, 0xe3, 0x2,
    0x11, 0x101, 0x8, 0x44, 0x83, 0x100, 0x43, 0xe3,
    0x1c, 0xf0, 0x86, 0x6a, 0x6b, 0xf, 0x3, 0x2d,
    0x86, 0x17, 0x7b, 0x10, 0xf6, 0x80, 0x78, 0x7a,
    0xa1, 0xe1, 0xef, 0x8c, 0xf6, 0x87, 0x4b, 0xa7,
    0xe2, 0x77, 0xfa, 0xb8, 0x81, 0xee, 0x77, 0xc0,
    0x9d, 0x29, 0x20, 0x27, 0x71, 0x12, 0xe0, 0x6b,
    0xd1, 0x7c, 0xa, 0x89, 0x7d, 0x87, 0xc4, 0x101,
    0xc1, 0x31, 0xaf, 0x38, 0x3, 0x68, 0x1b, 0x76,
    0x79, 0x3f, 0xdb, 0xc7, 0x1b, 0x36, 0x7b, 0xe2,
    0x63, 0x81, 0xee, 0xc, 0x63, 0x8b, 0x78, 0x38,
    0x97, 0x9b, 0xd7, 0x8f, 0xdd, 0xf2, 0xa3, 0x77,
    0x8c, 0xc3, 0x39, 0x20, 0xb3, 0x12, 0x11, 0xe,
    0x17, 0x42, 0x80, 0x2c, 0xc4, 0x92, 0x59, 0xc8,
    0xdb, 0x40, 0x76, 0x64, 0xb4, 0x55, 0x1a, 0x9e,
    0xfe, 0x5f, 0x6, 0x3c, 0x41, 0xef, 0xd4, 0xaa,
    0x98, 0x29, 0xcd, 0x1f, 0x2, 0xa8, 0x87, 0xd2,
    0xa0, 0x93, 0x98, 0xef, 0xc, 0x43, 0xed, 0x9d,
    0xc2, 0xeb, 0x81, 0xe9, 0x64, 0x23, 0x68, 0x1e,
    0x25, 0x57, 0xde, 0x9a, 0xcf, 0x7f, 0xe5, 0xba,
    0x41, 0xea, 0xea, 0x36, 0x1a, 0x28, 0x79, 0x20,
    0x5e, 0x18, 0x4e, 0x7c, 0x8e, 0x58, 0x7a, 0xef,
    0x91, 0x2, 0x93, 0xbb, 0x56, 0xa1, 0x49, 0x1b,
    0x79, 0x92, 0xf3, 0x58, 0x4f, 0x52, 0x9c, 0x2,
    0x77, 0xaf, 0x2a, 0x8f, 0x49, 0xd0, 0x99, 0x4d,
    0x98, 0x101, 0x60, 0x93, 0x100, 0x75, 0x31, 0xce,
    0x49, 0x20, 0x56, 0x57, 0xe2, 0xf5, 0x26, 0x2b,
    0x8a, 0xbf, 0xde, 0xd0, 0x83, 0x34, 0xf4, 0x17
};

// ============================================================================
// arsenic_model implementation
// ============================================================================

void stuffit_arsenic_decompressor::arsenic_model::init(int first_sym, int last_sym, int inc, int freq_limit) {
    increment = inc;
    frequency_limit = freq_limit;
    num_symbols = static_cast<size_t>(last_sym - first_sym + 1);
    for (size_t i = 0; i < num_symbols; i++) {
        symbols[i].symbol = static_cast<int>(i) + first_sym;
        symbols[i].frequency = increment;
    }
    total_frequency = increment * static_cast<int>(num_symbols);
}

void stuffit_arsenic_decompressor::arsenic_model::reset_frequencies() {
    total_frequency = increment * static_cast<int>(num_symbols);
    for (size_t i = 0; i < num_symbols; i++) {
        symbols[i].frequency = increment;
    }
}

void stuffit_arsenic_decompressor::arsenic_model::increase_frequency(size_t idx) {
    symbols[idx].frequency += increment;
    total_frequency += increment;

    if (total_frequency > frequency_limit) {
        total_frequency = 0;
        for (size_t i = 0; i < num_symbols; i++) {
            symbols[i].frequency++;
            symbols[i].frequency >>= 1;
            total_frequency += symbols[i].frequency;
        }
    }
}

// ============================================================================
// mtf_decoder implementation
// ============================================================================

stuffit_arsenic_decompressor::mtf_decoder::mtf_decoder() {
    reset();
}

void stuffit_arsenic_decompressor::mtf_decoder::reset() {
    for (size_t i = 0; i < 256; i++) {
        table_[i] = static_cast<u8>(i);
    }
}

u8 stuffit_arsenic_decompressor::mtf_decoder::decode(size_t symbol) {
    u8 val = table_[symbol];
    // Move to front
    for (size_t i = symbol; i > 0; i--) {
        table_[i] = table_[i - 1];
    }
    table_[0] = val;
    return val;
}

// ============================================================================
// Bit buffer operations
// ============================================================================

void stuffit_arsenic_decompressor::refill_bit_buffer() {
    while (bits_in_buffer_ < 24 && in_ptr_ < in_end_) {
        bit_buffer_ = (bit_buffer_ << 8) | static_cast<u32>(*in_ptr_++);
        bits_in_buffer_ += 8;
    }
}

int stuffit_arsenic_decompressor::read_bit_be() {
    if (bits_in_buffer_ == 0) {
        refill_bit_buffer();
        if (bits_in_buffer_ == 0) return 0;  // Use 0 if truly no more bits
    }
    bits_in_buffer_--;
    return (bit_buffer_ >> bits_in_buffer_) & 1;
}

// ============================================================================
// Arithmetic decoder operations
// ============================================================================

void stuffit_arsenic_decompressor::read_next_code(i32 sym_low, i32 sym_size, i32 sym_tot) {
    i32 renorm_factor = range_ / sym_tot;
    i32 low_incr = renorm_factor * sym_low;

    code_ -= low_incr;
    if (sym_low + sym_size == sym_tot) {
        range_ -= low_incr;
    } else {
        range_ = sym_size * renorm_factor;
    }

    while (range_ <= HALF) {
        range_ <<= 1;
        int bit = read_bit_be();
        if (bit < 0) bit = 0;  // Use 0 if no more bits
        code_ = (code_ << 1) | bit;
    }
}

int stuffit_arsenic_decompressor::decode_symbol(arsenic_model& model) {
    i32 freq = code_ / (range_ / model.total_frequency);
    i32 cumulative = 0;
    size_t n = 0;
    for (n = 0; n < model.num_symbols - 1; n++) {
        if (cumulative + model.symbols[n].frequency > freq) break;
        cumulative += model.symbols[n].frequency;
    }

    read_next_code(cumulative, model.symbols[n].frequency, model.total_frequency);
    model.increase_frequency(n);

    return model.symbols[n].symbol;
}

int stuffit_arsenic_decompressor::decode_bit_string(arsenic_model& model, int bits) {
    int result = 0;
    for (int i = 0; i < bits; i++) {
        if (decode_symbol(model)) result |= (1 << i);
    }
    return result;
}

// ============================================================================
// BWT inverse
// ============================================================================

void stuffit_arsenic_decompressor::calculate_inverse_bwt() {
    std::array<size_t, 256> count{};
    for (size_t i = 0; i < num_bytes_; i++) {
        count[block_[i]]++;
    }

    // Calculate cumulative counts
    size_t sum = 0;
    for (size_t i = 0; i < 256; i++) {
        size_t tmp = count[i];
        count[i] = sum;
        sum += tmp;
    }

    // Build transform table
    for (size_t i = 0; i < num_bytes_; i++) {
        transform_[count[block_[i]]++] = static_cast<u32>(i);
    }
}

// ============================================================================
// Main decompressor
// ============================================================================

stuffit_arsenic_decompressor::stuffit_arsenic_decompressor() {
    reset();
}

void stuffit_arsenic_decompressor::reset() {
    state_ = state::INIT_DECODER;
    bit_buffer_ = 0;
    bits_in_buffer_ = 0;

    range_ = ONE;
    code_ = 0;
    init_bits_read_ = 0;
    decoder_initialized_ = false;

    // Initialize models
    initial_model_.init(0, 1, 1, 256);
    selector_model_.init(0, 10, 8, 1024);
    mtf_models_[0].init(2, 3, 8, 1024);
    mtf_models_[1].init(4, 7, 4, 1024);
    mtf_models_[2].init(8, 15, 4, 1024);
    mtf_models_[3].init(16, 31, 4, 1024);
    mtf_models_[4].init(32, 63, 2, 1024);
    mtf_models_[5].init(64, 127, 2, 1024);
    mtf_models_[6].init(128, 255, 1, 1024);

    mtf_.reset();

    block_bits_ = 0;
    block_size_ = 0;
    end_of_blocks_ = false;

    randomized_ = false;
    transform_index_ = 0;
    num_bytes_ = 0;
    block_.clear();
    transform_.clear();

    selector_value_ = -1;
    zero_state_ = 0;
    zero_count_ = 0;
    in_zero_run_ = false;

    output_pos_ = 0;
    rand_index_ = 0;
    rand_count_ = 0;
    byte_count_ = 0;
    count_state_ = 0;
    last_byte_ = 0;
    repeat_remaining_ = 0;

    header_char_ = 0;
    bit_string_bits_remaining_ = 0;
    bit_string_value_ = 0;
}

result_t<stream_result> stuffit_arsenic_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    // Set input pointers for bit buffer refilling
    in_ptr_ = input.data();
    in_end_ = input.data() + input.size();
    byte* out_ptr = output.data();
    byte* out_end = output.data() + output.size();

    auto bytes_read = [&]() -> size_t {
        return static_cast<size_t>(in_ptr_ - input.data());
    };
    auto bytes_written = [&]() -> size_t {
        return static_cast<size_t>(out_ptr - output.data());
    };

    // Initial bit buffer fill
    refill_bit_buffer();

    while (state_ != state::DONE) {
        // Check if we need more input (except for states that don't need them)
        if (state_ != state::OUTPUT_BLOCK && state_ != state::PROCESS_BLOCK) {
            if (bits_in_buffer_ == 0 && in_ptr_ >= in_end_) {
                if (input_finished) {
                    state_ = state::DONE;
                    return stream_result::done(bytes_read(), bytes_written());
                }
                return stream_result::need_input(bytes_read(), bytes_written());
            }
        }

        switch (state_) {
            case state::INIT_DECODER: {
                // Initialize arithmetic decoder - read NUM_BITS bits
                while (init_bits_read_ < NUM_BITS) {
                    int bit = read_bit_be();
                    code_ = (code_ << 1) | bit;
                    init_bits_read_++;
                }
                decoder_initialized_ = true;
                state_ = state::READ_HEADER_A;
                break;
            }

            case state::READ_HEADER_A: {
                header_char_ = decode_bit_string(initial_model_, 8);
                if (header_char_ != 'A') {
                    return std::unexpected(error{error_code::CorruptData, "Invalid Arsenic header (expected 'A')"});
                }
                state_ = state::READ_HEADER_S;
                break;
            }

            case state::READ_HEADER_S: {
                header_char_ = decode_bit_string(initial_model_, 8);
                if (header_char_ != 's') {
                    return std::unexpected(error{error_code::CorruptData, "Invalid Arsenic header (expected 's')"});
                }
                state_ = state::READ_BLOCK_BITS;
                break;
            }

            case state::READ_BLOCK_BITS: {
                block_bits_ = decode_bit_string(initial_model_, 4) + 9;
                block_size_ = static_cast<size_t>(1) << block_bits_;
                block_.resize(block_size_);
                transform_.resize(block_size_);
                state_ = state::READ_END_FLAG;
                break;
            }

            case state::READ_END_FLAG: {
                end_of_blocks_ = decode_symbol(initial_model_) != 0;
                if (end_of_blocks_) {
                    state_ = state::READ_CRC;
                } else {
                    state_ = state::READ_BLOCK_HEADER;
                }
                break;
            }

            case state::READ_BLOCK_HEADER: {
                mtf_.reset();
                randomized_ = decode_symbol(initial_model_) != 0;
                transform_index_ = static_cast<size_t>(decode_bit_string(initial_model_, block_bits_));
                num_bytes_ = 0;
                selector_value_ = -1;
                in_zero_run_ = false;
                state_ = state::READ_BLOCK_DATA;
                break;
            }

            case state::READ_BLOCK_DATA: {
                while (num_bytes_ < block_size_) {
                    // Handle pending zero run
                    if (in_zero_run_ && zero_count_ > 0) {
                        u8 val = mtf_.decode(0);
                        while (zero_count_ > 0 && num_bytes_ < block_size_) {
                            block_[num_bytes_++] = val;
                            zero_count_--;
                        }
                        if (zero_count_ > 0) continue;
                        in_zero_run_ = false;
                    }

                    // Get selector if we don't have one
                    if (selector_value_ < 0) {
                        selector_value_ = decode_symbol(selector_model_);
                    }

                    int sel = selector_value_;

                    if (sel == 0 || sel == 1) {
                        // Zero counting mode
                        if (!in_zero_run_) {
                            zero_state_ = 1;
                            zero_count_ = 0;
                            in_zero_run_ = true;
                        }

                        while (sel < 2) {
                            if (sel == 0) zero_count_ += zero_state_;
                            else if (sel == 1) zero_count_ += 2 * zero_state_;
                            zero_state_ *= 2;
                            sel = decode_symbol(selector_model_);
                        }
                        selector_value_ = sel;
                        // Continue to process zero_count_
                        continue;
                    }

                    // Handle non-zero symbols
                    if (sel == 10) {
                        // End of block
                        selector_value_ = -1;
                        break;
                    }

                    size_t symbol;
                    if (sel == 2) {
                        symbol = 1;
                    } else {
                        symbol = static_cast<size_t>(decode_symbol(mtf_models_[static_cast<size_t>(sel - 3)]));
                    }

                    if (num_bytes_ >= block_size_) break;
                    block_[num_bytes_++] = mtf_.decode(symbol);
                    selector_value_ = -1;  // Need new selector
                }

                if (transform_index_ >= num_bytes_ && num_bytes_ > 0) {
                    return std::unexpected(error{error_code::CorruptData, "Invalid transform index"});
                }

                // Reset models for next block
                selector_model_.reset_frequencies();
                for (auto& m : mtf_models_) m.reset_frequencies();

                state_ = state::PROCESS_BLOCK;
                break;
            }

            case state::PROCESS_BLOCK: {
                // Inverse BWT
                calculate_inverse_bwt();

                // Initialize output state
                output_pos_ = 0;
                rand_index_ = 0;
                rand_count_ = RandomizationTable[0];
                byte_count_ = 0;
                count_state_ = 0;
                repeat_remaining_ = 0;

                state_ = state::OUTPUT_BLOCK;
                break;
            }

            case state::OUTPUT_BLOCK: {
                while (static_cast<size_t>(byte_count_) < num_bytes_ || repeat_remaining_ > 0) {
                    if (out_ptr >= out_end) {
                        return stream_result::need_output(bytes_read(), bytes_written());
                    }

                    // Handle pending repeats from RLE
                    if (repeat_remaining_ > 0) {
                        *out_ptr++ = static_cast<byte>(last_byte_);
                        repeat_remaining_--;
                        continue;
                    }

                    if (static_cast<size_t>(byte_count_) >= num_bytes_) break;

                    transform_index_ = transform_[transform_index_];
                    int b = block_[transform_index_];

                    if (randomized_ && rand_count_ == byte_count_) {
                        b ^= 1;
                        rand_index_ = (rand_index_ + 1) & 255;
                        rand_count_ += RandomizationTable[rand_index_];
                    }

                    byte_count_++;

                    if (count_state_ == 4) {
                        count_state_ = 0;
                        if (b == 0) continue;
                        // Output 'b' more copies of 'last_byte_'
                        repeat_remaining_ = b;
                        continue;
                    } else {
                        if (b == last_byte_) count_state_++;
                        else {
                            count_state_ = 1;
                            last_byte_ = b;
                        }
                        *out_ptr++ = static_cast<byte>(b);
                    }
                }

                // Check for end of blocks
                state_ = state::READ_END_FLAG;
                break;
            }

            case state::READ_CRC: {
                // Read and discard 32-bit CRC
                decode_bit_string(initial_model_, 32);
                state_ = state::DONE;
                break;
            }

            case state::DONE:
                break;
        }
    }

    return stream_result::done(bytes_read(), bytes_written());
}

}  // namespace crate
