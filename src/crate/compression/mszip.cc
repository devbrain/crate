#include <crate/compression/mszip.hh>

namespace crate {

mszip_decompressor::mszip_decompressor() {
    init_state();
}

void mszip_decompressor::init_state() {
    state_ = state::READ_SIGNATURE_0;
    bit_buffer_ = 0;
    bits_left_ = 0;
    final_block_ = false;
    block_type_ = 0;
    stored_len_ = 0;
    stored_remaining_ = 0;
    hlit_ = 0;
    hdist_ = 0;
    hclen_ = 0;
    code_len_idx_ = 0;
    code_len_lengths_.fill(0);
    all_lengths_.clear();
    length_decode_idx_ = 0;
    last_length_ = 0;
    repeat_count_ = 0;
    repeat_value_ = 0;
    current_symbol_ = 0;
    match_length_ = 0;
    match_distance_ = 0;
    match_remaining_ = 0;
    history_pos_ = 0;
    history_.fill(0);
}

void mszip_decompressor::reset() {
    init_state();
}

void mszip_decompressor::update_history(u8 value) {
    history_[history_pos_++ & (MSZIP_BLOCK_SIZE - 1)] = value;
}

u8 mszip_decompressor::get_history(size_t distance) const {
    return history_[(history_pos_ - distance) & (MSZIP_BLOCK_SIZE - 1)];
}

void mszip_decompressor::build_fixed_tables() {
    // Fixed literal/length code lengths (RFC 1951)
    std::array<u8, 288> lit_lengths{};
    std::fill(lit_lengths.begin(), lit_lengths.begin() + 144, 8);
    std::fill(lit_lengths.begin() + 144, lit_lengths.begin() + 256, 9);
    std::fill(lit_lengths.begin() + 256, lit_lengths.begin() + 280, 7);
    std::fill(lit_lengths.begin() + 280, lit_lengths.end(), 8);
    literal_decoder_.build(lit_lengths);

    // Fixed distance code lengths
    std::array<u8, 32> dist_lengths{};
    std::fill(dist_lengths.begin(), dist_lengths.end(), 5);
    distance_decoder_.build(dist_lengths);
}

bool mszip_decompressor::try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
    // Fill bit buffer until we have enough bits
    while (bits_left_ < n) {
        if (ptr >= end) {
            return false;  // Need more input
        }
        bit_buffer_ |= static_cast<u64>(*ptr++) << bits_left_;
        bits_left_ += 8;
    }

    // Extract bits (LSB first)
    out = static_cast<u32>(bit_buffer_ & ((1ULL << n) - 1));
    bit_buffer_ >>= n;
    bits_left_ -= n;
    return true;
}

bool mszip_decompressor::try_read_byte(const byte*& ptr, const byte* end, u8& out) {
    // If we have bits in the buffer, use them
    if (bits_left_ >= 8) {
        out = static_cast<u8>(bit_buffer_ & 0xFF);
        bit_buffer_ >>= 8;
        bits_left_ -= 8;
        return true;
    }

    // Otherwise read directly
    if (ptr >= end) {
        return false;
    }
    out = *ptr++;
    return true;
}

template<size_t N>
bool mszip_decompressor::try_decode(const byte*& ptr, const byte* end,
                                     huffman_decoder<N>& decoder, u16& out) {
    // We need to peek enough bits to decode
    // First ensure we have at least 15 bits (max Huffman code length)
    constexpr unsigned MAX_BITS = 15;

    while (bits_left_ < MAX_BITS && ptr < end) {
        bit_buffer_ |= static_cast<u64>(*ptr++) << bits_left_;
        bits_left_ += 8;
    }

    if (bits_left_ == 0) {
        return false;
    }

    // Create a small buffer from our bits for the decoder
    std::array<byte, 8> temp_buf{};
    u64 temp_bits = bit_buffer_;
    for (size_t i = 0; i < 8 && i * 8 < bits_left_; i++) {
        temp_buf[i] = static_cast<byte>(temp_bits & 0xFF);
        temp_bits >>= 8;
    }

    size_t temp_size = (bits_left_ + 7) / 8;
    lsb_bitstream bs(byte_span(temp_buf.data(), temp_size));

    auto result = decoder.decode(bs);
    if (!result) {
        // Need more input or error
        return false;
    }

    // Remove the consumed bits
    unsigned consumed = static_cast<unsigned>(bs.byte_position() * 8) - (8 - (bs.remaining_bits() % 8)) % 8;
    // Actually, calculate how many bits were consumed
    size_t original_bits = temp_size * 8;
    size_t remaining = bs.remaining_bits();
    consumed = static_cast<unsigned>(original_bits - remaining);

    if (consumed > bits_left_) {
        return false;  // Something went wrong
    }

    bit_buffer_ >>= consumed;
    bits_left_ -= consumed;
    out = *result;
    return true;
}

result_t<stream_result> mszip_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    const byte* in_ptr = input.data();
    const byte* in_end = input.data() + input.size();
    size_t out_pos = 0;

    u32 bits;
    u8 byte_val;
    u16 symbol;

    while (state_ != state::DONE) {
        switch (state_) {
            case state::READ_SIGNATURE_0:
                if (!try_read_byte(in_ptr, in_end, byte_val)) {
                    goto need_input;
                }
                if (byte_val != 'C') {
                    return std::unexpected(error{error_code::InvalidSignature,
                        "Missing MSZIP 'CK' signature"});
                }
                state_ = state::READ_SIGNATURE_1;
                break;

            case state::READ_SIGNATURE_1:
                if (!try_read_byte(in_ptr, in_end, byte_val)) {
                    goto need_input;
                }
                if (byte_val != 'K') {
                    return std::unexpected(error{error_code::InvalidSignature,
                        "Missing MSZIP 'CK' signature"});
                }
                state_ = state::READ_BLOCK_HEADER;
                break;

            case state::READ_BLOCK_HEADER:
                if (!try_read_bits(in_ptr, in_end, 3, bits)) {
                    goto need_input;
                }
                final_block_ = (bits & 1) != 0;
                block_type_ = static_cast<u8>((bits >> 1) & 3);

                switch (block_type_) {
                    case 0:
                        state_ = state::ALIGN_STORED;
                        break;
                    case 1:
                        state_ = state::BUILD_FIXED_TABLES;
                        break;
                    case 2:
                        state_ = state::READ_HLIT;
                        break;
                    default:
                        return std::unexpected(error{error_code::InvalidBlockType,
                            "Reserved block type 3"});
                }
                break;

            // ===== Stored block handling =====
            case state::ALIGN_STORED:
                // Discard bits to align to byte boundary
                bits_left_ = 0;
                bit_buffer_ = 0;
                state_ = state::READ_STORED_LEN_LO;
                break;

            case state::READ_STORED_LEN_LO:
                if (!try_read_byte(in_ptr, in_end, byte_val)) {
                    goto need_input;
                }
                stored_len_ = byte_val;
                state_ = state::READ_STORED_LEN_HI;
                break;

            case state::READ_STORED_LEN_HI:
                if (!try_read_byte(in_ptr, in_end, byte_val)) {
                    goto need_input;
                }
                stored_len_ |= static_cast<u16>(byte_val) << 8;
                state_ = state::READ_STORED_NLEN_LO;
                break;

            case state::READ_STORED_NLEN_LO:
                if (!try_read_byte(in_ptr, in_end, byte_val)) {
                    goto need_input;
                }
                stored_remaining_ = byte_val;  // Temporarily store NLEN low
                state_ = state::READ_STORED_NLEN_HI;
                break;

            case state::READ_STORED_NLEN_HI: {
                if (!try_read_byte(in_ptr, in_end, byte_val)) {
                    goto need_input;
                }
                u16 nlen = stored_remaining_ | (static_cast<u16>(byte_val) << 8);
                if ((stored_len_ ^ nlen) != 0xFFFF) {
                    return std::unexpected(error{error_code::CorruptData,
                        "Invalid stored block length"});
                }
                stored_remaining_ = stored_len_;
                state_ = state::COPY_STORED;
                break;
            }

            case state::COPY_STORED:
                while (stored_remaining_ > 0) {
                    if (out_pos >= output.size()) {
                        goto need_output;
                    }
                    if (!try_read_byte(in_ptr, in_end, byte_val)) {
                        goto need_input;
                    }
                    output[out_pos++] = byte_val;
                    update_history(byte_val);
                    stored_remaining_--;
                }
                // Block complete
                if (final_block_) {
                    state_ = state::DONE;
                } else {
                    state_ = state::READ_BLOCK_HEADER;
                }
                break;

            // ===== Fixed Huffman =====
            case state::BUILD_FIXED_TABLES:
                build_fixed_tables();
                state_ = state::DECODE_SYMBOL;
                break;

            // ===== Dynamic Huffman table reading =====
            case state::READ_HLIT:
                if (!try_read_bits(in_ptr, in_end, 5, bits)) {
                    goto need_input;
                }
                hlit_ = static_cast<u16>(bits + 257);
                state_ = state::READ_HDIST;
                break;

            case state::READ_HDIST:
                if (!try_read_bits(in_ptr, in_end, 5, bits)) {
                    goto need_input;
                }
                hdist_ = static_cast<u16>(bits + 1);
                state_ = state::READ_HCLEN;
                break;

            case state::READ_HCLEN:
                if (!try_read_bits(in_ptr, in_end, 4, bits)) {
                    goto need_input;
                }
                hclen_ = static_cast<u16>(bits + 4);
                code_len_idx_ = 0;
                code_len_lengths_.fill(0);
                state_ = state::READ_CODE_LENGTHS;
                break;

            case state::READ_CODE_LENGTHS:
                while (code_len_idx_ < hclen_) {
                    if (!try_read_bits(in_ptr, in_end, 3, bits)) {
                        goto need_input;
                    }
                    code_len_lengths_[deflate::code_length_order[code_len_idx_]] =
                        static_cast<u8>(bits);
                    code_len_idx_++;
                }
                state_ = state::BUILD_CODE_LEN_TABLE;
                break;

            case state::BUILD_CODE_LEN_TABLE: {
                auto result = code_len_decoder_.build(code_len_lengths_);
                if (!result) {
                    return std::unexpected(result.error());
                }
                all_lengths_.resize(hlit_ + hdist_);
                std::fill(all_lengths_.begin(), all_lengths_.end(), 0);
                length_decode_idx_ = 0;
                last_length_ = 0;
                repeat_count_ = 0;
                state_ = state::DECODE_LENGTH_CODES;
                break;
            }

            case state::DECODE_LENGTH_CODES:
                while (length_decode_idx_ < all_lengths_.size()) {
                    // Handle pending repeats first
                    while (repeat_count_ > 0 && length_decode_idx_ < all_lengths_.size()) {
                        all_lengths_[length_decode_idx_++] = repeat_value_;
                        repeat_count_--;
                    }

                    if (length_decode_idx_ >= all_lengths_.size()) {
                        break;
                    }

                    if (!try_decode(in_ptr, in_end, code_len_decoder_, symbol)) {
                        goto need_input;
                    }

                    if (symbol < 16) {
                        all_lengths_[length_decode_idx_++] = static_cast<u8>(symbol);
                        last_length_ = static_cast<u8>(symbol);
                    } else if (symbol == 16) {
                        if (!try_read_bits(in_ptr, in_end, 2, bits)) {
                            goto need_input;
                        }
                        repeat_count_ = static_cast<u16>(bits + 3);
                        repeat_value_ = last_length_;
                    } else if (symbol == 17) {
                        if (!try_read_bits(in_ptr, in_end, 3, bits)) {
                            goto need_input;
                        }
                        repeat_count_ = static_cast<u16>(bits + 3);
                        repeat_value_ = 0;
                    } else {  // symbol == 18
                        if (!try_read_bits(in_ptr, in_end, 7, bits)) {
                            goto need_input;
                        }
                        repeat_count_ = static_cast<u16>(bits + 11);
                        repeat_value_ = 0;
                    }
                }
                state_ = state::BUILD_TABLES;
                break;

            case state::BUILD_TABLES: {
                auto lit_result = literal_decoder_.build(
                    std::span(all_lengths_.data(), hlit_));
                if (!lit_result) {
                    return std::unexpected(lit_result.error());
                }
                auto dist_result = distance_decoder_.build(
                    std::span(all_lengths_.data() + hlit_, hdist_));
                if (!dist_result) {
                    return std::unexpected(dist_result.error());
                }
                state_ = state::DECODE_SYMBOL;
                break;
            }

            // ===== Huffman decompression =====
            case state::DECODE_SYMBOL:
                if (!try_decode(in_ptr, in_end, literal_decoder_, symbol)) {
                    goto need_input;
                }
                current_symbol_ = symbol;

                if (symbol < 256) {
                    // Literal byte
                    if (out_pos >= output.size()) {
                        // Need to output this byte next time
                        goto need_output;
                    }
                    output[out_pos++] = static_cast<u8>(symbol);
                    update_history(static_cast<u8>(symbol));
                    // Stay in DECODE_SYMBOL state
                } else if (symbol == 256) {
                    // End of block
                    if (final_block_) {
                        state_ = state::DONE;
                    } else {
                        state_ = state::READ_BLOCK_HEADER;
                    }
                } else {
                    // Length code
                    unsigned len_idx = symbol - 257;
                    if (len_idx >= deflate::length_base.size()) {
                        return std::unexpected(error{error_code::InvalidMatchLength});
                    }
                    match_length_ = deflate::length_base[len_idx];

                    unsigned extra = deflate::length_extra_bits[len_idx];
                    if (extra > 0) {
                        state_ = state::READ_LENGTH_EXTRA;
                    } else {
                        state_ = state::DECODE_DISTANCE;
                    }
                }
                break;

            case state::READ_LENGTH_EXTRA: {
                unsigned len_idx = current_symbol_ - 257;
                unsigned extra = deflate::length_extra_bits[len_idx];
                if (!try_read_bits(in_ptr, in_end, extra, bits)) {
                    goto need_input;
                }
                match_length_ = static_cast<u16>(deflate::length_base[len_idx] + bits);
                state_ = state::DECODE_DISTANCE;
                break;
            }

            case state::DECODE_DISTANCE:
                if (!try_decode(in_ptr, in_end, distance_decoder_, symbol)) {
                    goto need_input;
                }
                if (symbol >= deflate::distance_base.size()) {
                    return std::unexpected(error{error_code::InvalidMatchDistance});
                }
                match_distance_ = deflate::distance_base[symbol];

                if (deflate::distance_extra_bits[symbol] > 0) {
                    current_symbol_ = symbol;  // Save for extra bits
                    state_ = state::READ_DISTANCE_EXTRA;
                } else {
                    match_remaining_ = match_length_;
                    state_ = state::COPY_MATCH;
                }
                break;

            case state::READ_DISTANCE_EXTRA: {
                unsigned extra = deflate::distance_extra_bits[current_symbol_];
                if (!try_read_bits(in_ptr, in_end, extra, bits)) {
                    goto need_input;
                }
                match_distance_ = static_cast<u16>(
                    deflate::distance_base[current_symbol_] + bits);
                match_remaining_ = match_length_;
                state_ = state::COPY_MATCH;
                break;
            }

            case state::COPY_MATCH:
                while (match_remaining_ > 0) {
                    if (out_pos >= output.size()) {
                        goto need_output;
                    }
                    u8 value = get_history(match_distance_);
                    output[out_pos++] = value;
                    update_history(value);
                    match_remaining_--;
                }
                state_ = state::DECODE_SYMBOL;
                break;

            case state::DONE:
                break;
        }
    }

    // Successfully completed
    {
        size_t bytes_read = static_cast<size_t>(in_ptr - input.data());
        return stream_result::done(bytes_read, out_pos);
    }

need_output:
    {
        size_t bytes_read = static_cast<size_t>(in_ptr - input.data());
        return stream_result::need_output(bytes_read, out_pos);
    }

need_input:
    if (input_finished) {
        // No more input coming
        if (state_ == state::DECODE_SYMBOL || state_ == state::READ_BLOCK_HEADER) {
            // Acceptable end points
            state_ = state::DONE;
            size_t bytes_read = static_cast<size_t>(in_ptr - input.data());
            return stream_result::done(bytes_read, out_pos);
        }
        return std::unexpected(error{error_code::TruncatedArchive,
            "Unexpected end of MSZIP data"});
    }
    {
        size_t bytes_read = static_cast<size_t>(in_ptr - input.data());
        return stream_result::need_input(bytes_read, out_pos);
    }
}

// Explicit template instantiation
template bool mszip_decompressor::try_decode<288>(
    const byte*& ptr, const byte* end, huffman_decoder<288>& decoder, u16& out);
template bool mszip_decompressor::try_decode<32>(
    const byte*& ptr, const byte* end, huffman_decoder<32>& decoder, u16& out);
template bool mszip_decompressor::try_decode<19>(
    const byte*& ptr, const byte* end, huffman_decoder<19>& decoder, u16& out);

}  // namespace crate
