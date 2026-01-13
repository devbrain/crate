#include <crate/compression/lzh.hh>

namespace crate {

void lzh_huffman_tree::clear() {
    codes_.clear();
    lengths_.clear();
    lookup_.fill(0xFFFF);
    max_length_ = 0;
}
bool lzh_huffman_tree::build_from_lengths(const std::vector<unsigned>& lengths) {
    clear();
    if (lengths.empty())
        return false;

    unsigned num_codes = static_cast<unsigned>(lengths.size());
    lengths_.resize(num_codes);
    codes_.resize(num_codes);

    // Find max length and count codes per length
    std::array<unsigned, MAX_CODE_LENGTH + 1> length_count{};
    max_length_ = 0;
    for (unsigned i = 0; i < num_codes; i++) {
        lengths_[i] = lengths[i];
        if (lengths[i] > 0) {
            if (lengths[i] > MAX_CODE_LENGTH)
                return false;
            length_count[lengths[i]]++;
            max_length_ = std::max(max_length_, lengths[i]);
        }
    }

    if (max_length_ == 0)
        return false;

    // Generate canonical codes
    std::array<unsigned, MAX_CODE_LENGTH + 1> next_code{};
    unsigned code = 0;
    for (unsigned bits = 1; bits <= max_length_; bits++) {
        code = (code + length_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    // Assign codes to symbols
    for (unsigned i = 0; i < num_codes; i++) {
        if (lengths_[i] > 0) {
            codes_[i] = next_code[lengths_[i]]++;
        }
    }

    // Build lookup table for fast decoding (first 8 bits)
    build_lookup_table();

    return true;
}
void lzh_huffman_tree::build_single(unsigned value) {
    clear();
    lengths_.resize(value + 1, 0);
    codes_.resize(value + 1, 0);
    lengths_[value] = 1;
    codes_[value] = 0;
    max_length_ = 1;
    single_value_ = value;
    is_single_ = true;
}
void lzh_huffman_tree::build_lookup_table() {
    lookup_.fill(0xFFFF);

    for (unsigned i = 0; i < codes_.size(); i++) {
        if (lengths_[i] > 0 && lengths_[i] <= 8) {
            // Fill all entries that match this code
            unsigned code = codes_[i];
            unsigned len = lengths_[i];
            unsigned shift = 8 - len;
            unsigned base = code << shift;
            unsigned count = 1u << shift;

            for (unsigned j = 0; j < count; j++) {
                unsigned entry = (len << 12) | i;
                lookup_[base + j] = static_cast<u16>(entry);
            }
        }
    }
}
lzh_decompressor::lzh_decompressor(lzh_format format) : format_(format) {
    switch (format) {
        case lzh_format::LH5:
            window_size_ = 8192;
            offsets_bits_ = 4;
            max_offset_codes_ = 14;
            break;
        case lzh_format::LH6:
            window_size_ = 32768;
            offsets_bits_ = 5;
            max_offset_codes_ = 16;
            break;
        case lzh_format::LH7:
            window_size_ = 65536;
            offsets_bits_ = 5;
            max_offset_codes_ = 17;
            break;
    }
    init_state();
}
result_t<size_t> lzh_decompressor::decompress(byte_span input, mutable_byte_span output) {
    msb_bitstream bs(input);
    size_t out_pos = 0;

    while (!bs.at_end() && out_pos < output.size()) {
        // Read block header
        auto block_result = process_block(bs, output, out_pos);
        if (!block_result) {
            if (out_pos > 0)
                break;  // EOF is OK if we have data
            return std::unexpected(block_result.error());
        }
        out_pos = *block_result;
    }

    return out_pos;
}
void lzh_decompressor::reset() {
    init_state();
}
void lzh_decompressor::init_state() {
    window_.resize(window_size_);
    std::fill(window_.begin(), window_.end(), 0);
    window_pos_ = 0;
}
}  // namespace crate
