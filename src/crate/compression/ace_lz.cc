#include <crate/compression/ace_lz.hh>
#include <algorithm>
#include <iostream>

namespace crate {

static bool g_debug = false;  // Set via ace_lz_debug()
void ace_lz_debug(bool enable) { g_debug = enable; }

void ace_lz_decompressor::init_state() {
    state_ = state::READ_MAIN_TREE_HEADER;
    bit_buffer_ = 0;
    bits_left_ = 0;

    main_tree_.clear();
    len_tree_.clear();
    width_tree_.clear();

    tree_num_widths_ = 0;
    tree_lower_width_ = 0;
    tree_upper_width_ = 0;
    tree_width_index_ = 0;
    tree_width_widths_.clear();
    tree_widths_.clear();

    syms_to_read_ = 0;
    current_symbol_ = 0;

    dist_hist_ = {0, 0, 0, 0};

    copy_dist_ = 0;
    copy_len_ = 0;
    copy_pos_ = 0;

    window_.clear();
    window_.reserve(dic_size_);
    window_pos_ = 0;
}

void ace_lz_decompressor::set_dictionary_size(size_t size) {
    dic_size_ = std::min(std::max(size, MINDICSIZE), MAXDICSIZE);
}

bool ace_lz_decompressor::try_peek_bits(const byte*& ptr, const byte* end, int n, u32& out) {
    // ACE uses big-endian bit reading with 32-bit word swapping
    // Keep unconsumed bits at MSB of 64-bit buffer
    auto n_unsigned = static_cast<unsigned>(n);
    while (bits_left_ < n_unsigned) {
        if (ptr + 4 <= end) {
            // Read 32-bit word in little-endian
            u32 word = static_cast<u32>(ptr[0]) |
                      (static_cast<u32>(ptr[1]) << 8) |
                      (static_cast<u32>(ptr[2]) << 16) |
                      (static_cast<u32>(ptr[3]) << 24);
            if (g_debug) {
                std::cerr << "Read word 0x" << std::hex << word << std::dec
                          << " at bits_left=" << bits_left_ << std::endl;
            }
            ptr += 4;
            // Add word to LSB side of buffer (existing bits stay at MSB)
            bit_buffer_ |= (static_cast<u64>(word) << (32 - bits_left_));
            bits_left_ += 32;
        } else if (ptr < end) {
            // Read remaining bytes
            u32 word = 0;
            unsigned bytes_read = 0;
            while (ptr < end && bytes_read < 4) {
                word |= static_cast<u32>(*ptr++) << (bytes_read * 8);
                bytes_read++;
            }
            bit_buffer_ |= (static_cast<u64>(word) << (32 - bits_left_));
            bits_left_ += bytes_read * 8;
        } else {
            return false;
        }
    }

    // Extract n bits from MSB
    out = static_cast<u32>(bit_buffer_ >> (64 - n_unsigned));
    if (g_debug) {
        std::cerr << "peek_bits(" << n << ")=" << out
                  << " buffer=0x" << std::hex << bit_buffer_ << std::dec
                  << " left=" << bits_left_ << std::endl;
    }
    return true;
}

bool ace_lz_decompressor::try_read_bits(const byte*& ptr, const byte* end, int n, u32& out) {
    if (!try_peek_bits(ptr, end, n, out)) {
        return false;
    }
    // Shift consumed bits out from MSB
    bit_buffer_ <<= n;
    bits_left_ -= static_cast<unsigned>(n);
    return true;
}

void ace_lz_decompressor::remove_bits(int n) {
    bit_buffer_ <<= n;
    bits_left_ -= static_cast<unsigned>(n);
}

bool ace_lz_decompressor::try_read_huffman_symbol(
    const byte*& ptr, const byte* end,
    const huffman_tree& tree, u16& symbol
) {
    u32 code = 0;
    if (!try_peek_bits(ptr, end, tree.max_width, code)) {
        return false;
    }

    if (code >= tree.codes.size()) {
        if (g_debug) std::cerr << "code " << code << " >= codes.size " << tree.codes.size() << std::endl;
        return false;
    }

    symbol = tree.codes[code];
    if (symbol >= tree.widths.size()) {
        if (g_debug) std::cerr << "symbol " << symbol << " >= widths.size " << tree.widths.size() << std::endl;
        return false;
    }

    int bits_consumed = tree.widths[symbol];
    if (g_debug && state_ == state::READ_MAIN_TREE_WIDTHS) {
        std::cerr << "[huff] code=" << code << " -> sym=" << symbol << " bits=" << bits_consumed << std::endl;
    }
    remove_bits(bits_consumed);
    return true;
}

// Quicksort matching the batch decoder's exact implementation
static void quicksort(std::vector<int>& keys, std::vector<int>& values, int left, int right) {
    if (left >= right)
        return;

    int new_left = left;
    int new_right = right;
    int m = keys[static_cast<size_t>(right)];

    while (true) {
        while (keys[static_cast<size_t>(new_left)] > m)
            new_left++;
        while (keys[static_cast<size_t>(new_right)] < m)
            new_right--;

        if (new_left <= new_right) {
            std::swap(keys[static_cast<size_t>(new_left)], keys[static_cast<size_t>(new_right)]);
            std::swap(values[static_cast<size_t>(new_left)], values[static_cast<size_t>(new_right)]);
            new_left++;
            new_right--;
        }

        if (new_left >= new_right)
            break;
    }

    if (left < new_right) {
        if (left < new_right - 1) {
            quicksort(keys, values, left, new_right);
        } else if (keys[static_cast<size_t>(left)] < keys[static_cast<size_t>(new_right)]) {
            std::swap(keys[static_cast<size_t>(left)], keys[static_cast<size_t>(new_right)]);
            std::swap(values[static_cast<size_t>(left)], values[static_cast<size_t>(new_right)]);
        }
    }

    if (right > new_left) {
        if (new_left < right - 1) {
            quicksort(keys, values, new_left, right);
        } else if (keys[static_cast<size_t>(new_left)] < keys[static_cast<size_t>(right)]) {
            std::swap(keys[static_cast<size_t>(new_left)], keys[static_cast<size_t>(right)]);
            std::swap(values[static_cast<size_t>(new_left)], values[static_cast<size_t>(right)]);
        }
    }
}

result_t<bool> ace_lz_decompressor::build_huffman_tree(
    std::vector<u8>& widths, int max_width, huffman_tree& tree
) {
    // Sort symbols by width (descending)
    std::vector<int> sorted_symbols(widths.size());
    std::vector<int> sorted_widths(widths.size());
    for (size_t i = 0; i < widths.size(); i++) {
        sorted_symbols[i] = static_cast<int>(i);
        sorted_widths[i] = widths[i];
    }

    if (sorted_widths.size() > 1) {
        quicksort(sorted_widths, sorted_symbols, 0, static_cast<int>(sorted_widths.size()) - 1);
    }

    // Count used (non-zero width) symbols
    size_t used = 0;
    while (used < sorted_widths.size() && sorted_widths[used] != 0) {
        used++;
    }

    // Handle edge cases
    if (used < 2) {
        if (!widths.empty() && !sorted_symbols.empty()) {
            widths[static_cast<size_t>(sorted_symbols[0])] = 1;
        }
        if (used == 0) used = 1;
    }

    sorted_symbols.resize(used);
    sorted_widths.resize(used);

    // Build codes table
    tree.max_width = max_width;
    tree.widths.assign(widths.begin(), widths.end());

    size_t max_codes = 1u << max_width;
    tree.codes.clear();
    tree.codes.reserve(max_codes);

    for (size_t idx = used; idx > 0; idx--) {
        size_t i = idx - 1;
        int sym = sorted_symbols[i];
        int wdt = sorted_widths[i];
        if (wdt > max_width) {
            return crate::make_unexpected(error{error_code::CorruptData, "Huffman: width > max_width"});
        }
        size_t repeat = 1u << (max_width - wdt);
        for (size_t j = 0; j < repeat; j++) {
            tree.codes.push_back(static_cast<u16>(sym));
        }
        if (tree.codes.size() > max_codes) {
            return crate::make_unexpected(error{error_code::CorruptData, "Huffman: codes > max_codes"});
        }
    }

    return true;
}

result_t<stream_result> ace_lz_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    if (!expected_output_set()) {
        return crate::make_unexpected(error{
            error_code::InvalidParameter,
            "Expected size required for bounded decompression"
        });
    }

    const byte* in_ptr = input.data();
    const byte* in_end = input.data() + input.size();
    byte* out_ptr = output.data();
    byte* out_end = output.data() + output.size();

    // Limit output to expected size
    size_t remaining = expected_output_size() - total_output_written();
    if (static_cast<size_t>(out_end - out_ptr) > remaining) {
        out_end = out_ptr + remaining;
    }

    auto bytes_read = [&]() -> size_t {
        return static_cast<size_t>(in_ptr - input.data());
    };
    auto bytes_written = [&]() -> size_t {
        return static_cast<size_t>(out_ptr - output.data());
    };
    auto finalize = [&](decode_status status) -> stream_result {
        size_t written = bytes_written();
        advance_output(written);

        if (state_ == state::DONE || total_output_written() >= expected_output_size()) {
            state_ = state::DONE;
            return stream_result::done(bytes_read(), written);
        }

        if (status == decode_status::needs_more_input) {
            return stream_result::need_input(bytes_read(), written);
        }
        if (status == decode_status::needs_more_output) {
            return stream_result::need_output(bytes_read(), written);
        }

        return stream_result::done(bytes_read(), written);
    };

    auto output_byte = [&](u8 b) {
        *out_ptr++ = b;
        window_.push_back(b);
        if (window_.size() > dic_size_) {
            window_.erase(window_.begin());
        }
    };

    while (state_ != state::DONE) {
        // Check if we've written enough
        if (total_output_written() + bytes_written() >= expected_output_size()) {
            state_ = state::DONE;
            break;
        }

        switch (state_) {
            case state::READ_MAIN_TREE_HEADER: {
                u32 num_widths = 0, lower_width = 0, upper_width = 0;
                if (!try_read_bits(in_ptr, in_end, 9, num_widths)) {
                    if (input_finished) {
                        state_ = state::DONE;
                        return finalize(decode_status::done);
                    }
                    return finalize(decode_status::needs_more_input);
                }
                if (!try_read_bits(in_ptr, in_end, 4, lower_width)) {
                    return finalize(decode_status::needs_more_input);
                }
                if (!try_read_bits(in_ptr, in_end, 4, upper_width)) {
                    return finalize(decode_status::needs_more_input);
                }

                if (g_debug) {
                    std::cerr << "MAIN tree header: num_widths=" << num_widths
                              << " lower=" << lower_width << " upper=" << upper_width << std::endl;
                }

                tree_num_widths_ = static_cast<int>(num_widths) + 1;
                if (tree_num_widths_ > NUMMAINCODES + 1) {
                    tree_num_widths_ = NUMMAINCODES + 1;
                }
                tree_lower_width_ = static_cast<int>(lower_width);
                tree_upper_width_ = static_cast<int>(upper_width);
                tree_width_widths_.clear();
                tree_width_index_ = 0;
                state_ = state::READ_MAIN_TREE_WIDTH_WIDTHS;
                break;
            }

            case state::READ_MAIN_TREE_WIDTH_WIDTHS: {
                while (tree_width_index_ < tree_upper_width_ + 1) {
                    u32 ww = 0;
                    if (!try_read_bits(in_ptr, in_end, WIDTHWIDTHBITS, ww)) {
                        return finalize(decode_status::needs_more_input);
                    }
                    tree_width_widths_.push_back(static_cast<u8>(ww));
                    tree_width_index_++;
                }
                state_ = state::BUILD_MAIN_WIDTH_TREE;
                break;
            }

            case state::BUILD_MAIN_WIDTH_TREE: {
                if (g_debug) {
                    std::cerr << "Width widths: ";
                    for (auto w : tree_width_widths_) std::cerr << static_cast<int>(w) << " ";
                    std::cerr << std::endl;
                }
                auto r = build_huffman_tree(tree_width_widths_, MAXWIDTHWIDTH, width_tree_);
                if (!r) return crate::make_unexpected(r.error());
                if (g_debug) {
                    std::cerr << "Width tree built, codes.size=" << width_tree_.codes.size()
                              << " max_width=" << width_tree_.max_width << std::endl;
                    std::cerr << "First 20 codes: ";
                    for (size_t i = 0; i < std::min<size_t>(20, width_tree_.codes.size()); i++) {
                        std::cerr << width_tree_.codes[i] << " ";
                    }
                    std::cerr << std::endl;
                }
                tree_widths_.clear();
                state_ = state::READ_MAIN_TREE_WIDTHS;
                break;
            }

            case state::READ_MAIN_TREE_WIDTHS: {
                while (static_cast<int>(tree_widths_.size()) < tree_num_widths_) {
                    u16 symbol = 0;
                    if (!try_read_huffman_symbol(in_ptr, in_end, width_tree_, symbol)) {
                        return finalize(decode_status::needs_more_input);
                    }

                    if (static_cast<int>(symbol) < tree_upper_width_) {
                        tree_widths_.push_back(static_cast<u8>(symbol));
                    } else {
                        // Run-length encoding of zeros
                        u32 extra = 0;
                        if (!try_read_bits(in_ptr, in_end, 4, extra)) {
                            return finalize(decode_status::needs_more_input);
                        }
                        int repeat = static_cast<int>(extra) + 4;
                        repeat = std::min(repeat, tree_num_widths_ - static_cast<int>(tree_widths_.size()));
                        for (int i = 0; i < repeat; i++) {
                            tree_widths_.push_back(0);
                        }
                    }
                }
                state_ = state::BUILD_MAIN_TREE;
                break;
            }

            case state::BUILD_MAIN_TREE: {
                if (g_debug) {
                    std::cerr << "Raw main tree widths (first 30): ";
                    for (size_t i = 0; i < std::min<size_t>(30, tree_widths_.size()); i++) {
                        std::cerr << static_cast<int>(tree_widths_[i]) << " ";
                    }
                    std::cerr << " ... (total " << tree_widths_.size() << ")" << std::endl;
                }
                // Delta decode widths
                if (tree_upper_width_ > 0) {
                    for (size_t i = 1; i < tree_widths_.size(); i++) {
                        tree_widths_[i] = static_cast<u8>((tree_widths_[i] + tree_widths_[i - 1]) % tree_upper_width_);
                    }
                }
                if (g_debug) {
                    std::cerr << "After delta decode (first 30): ";
                    for (size_t i = 0; i < std::min<size_t>(30, tree_widths_.size()); i++) {
                        std::cerr << static_cast<int>(tree_widths_[i]) << " ";
                    }
                    std::cerr << std::endl;
                }
                // Add lower_width offset
                for (auto& w : tree_widths_) {
                    if (w > 0) {
                        w = static_cast<u8>(w + tree_lower_width_);
                    }
                }
                if (g_debug) {
                    std::cerr << "After lower_width offset (first 30): ";
                    for (size_t i = 0; i < std::min<size_t>(30, tree_widths_.size()); i++) {
                        std::cerr << static_cast<int>(tree_widths_[i]) << " ";
                    }
                    std::cerr << std::endl;
                    int max_w = 0;
                    for (auto w : tree_widths_) max_w = std::max(max_w, static_cast<int>(w));
                    std::cerr << "Max width: " << max_w << " (max allowed: " << MAXCODEWIDTH << ")" << std::endl;
                }
                // Don't pad - use the exact number of widths read
                if (g_debug) {
                    std::cerr << "[STREAM] Before build: widths[32]=" << static_cast<int>(tree_widths_[32])
                              << " widths[105]=" << static_cast<int>(tree_widths_[105])
                              << " total=" << tree_widths_.size() << std::endl;
                }
                auto r = build_huffman_tree(tree_widths_, MAXCODEWIDTH, main_tree_);
                if (!r) return crate::make_unexpected(r.error());
                if (g_debug) {
                    std::cerr << "[STREAM] main_tree codes[1178]=" << main_tree_.codes[1178]
                              << " widths[32]=" << static_cast<int>(main_tree_.widths[32])
                              << " widths[105]=" << static_cast<int>(main_tree_.widths[105]) << std::endl;
                }
                state_ = state::READ_LEN_TREE_HEADER;
                break;
            }

            case state::READ_LEN_TREE_HEADER: {
                u32 num_widths = 0, lower_width = 0, upper_width = 0;
                if (!try_read_bits(in_ptr, in_end, 9, num_widths)) {
                    return finalize(decode_status::needs_more_input);
                }
                if (!try_read_bits(in_ptr, in_end, 4, lower_width)) {
                    return finalize(decode_status::needs_more_input);
                }
                if (!try_read_bits(in_ptr, in_end, 4, upper_width)) {
                    return finalize(decode_status::needs_more_input);
                }

                tree_num_widths_ = static_cast<int>(num_widths) + 1;
                if (tree_num_widths_ > NUMLENCODES + 1) {
                    tree_num_widths_ = NUMLENCODES + 1;
                }
                tree_lower_width_ = static_cast<int>(lower_width);
                tree_upper_width_ = static_cast<int>(upper_width);
                tree_width_widths_.clear();
                tree_width_index_ = 0;
                state_ = state::READ_LEN_TREE_WIDTH_WIDTHS;
                break;
            }

            case state::READ_LEN_TREE_WIDTH_WIDTHS: {
                while (tree_width_index_ < tree_upper_width_ + 1) {
                    u32 ww = 0;
                    if (!try_read_bits(in_ptr, in_end, WIDTHWIDTHBITS, ww)) {
                        return finalize(decode_status::needs_more_input);
                    }
                    tree_width_widths_.push_back(static_cast<u8>(ww));
                    tree_width_index_++;
                }
                state_ = state::BUILD_LEN_WIDTH_TREE;
                break;
            }

            case state::BUILD_LEN_WIDTH_TREE: {
                auto r = build_huffman_tree(tree_width_widths_, MAXWIDTHWIDTH, width_tree_);
                if (!r) return crate::make_unexpected(r.error());
                tree_widths_.clear();
                state_ = state::READ_LEN_TREE_WIDTHS;
                break;
            }

            case state::READ_LEN_TREE_WIDTHS: {
                while (static_cast<int>(tree_widths_.size()) < tree_num_widths_) {
                    u16 symbol = 0;
                    if (!try_read_huffman_symbol(in_ptr, in_end, width_tree_, symbol)) {
                        return finalize(decode_status::needs_more_input);
                    }

                    if (static_cast<int>(symbol) < tree_upper_width_) {
                        tree_widths_.push_back(static_cast<u8>(symbol));
                    } else {
                        // Run-length encoding of zeros
                        u32 extra = 0;
                        if (!try_read_bits(in_ptr, in_end, 4, extra)) {
                            return finalize(decode_status::needs_more_input);
                        }
                        int repeat = static_cast<int>(extra) + 4;
                        repeat = std::min(repeat, tree_num_widths_ - static_cast<int>(tree_widths_.size()));
                        for (int i = 0; i < repeat; i++) {
                            tree_widths_.push_back(0);
                        }
                    }
                }
                state_ = state::BUILD_LEN_TREE;
                break;
            }

            case state::BUILD_LEN_TREE: {
                // Delta decode widths
                if (tree_upper_width_ > 0) {
                    for (size_t i = 1; i < tree_widths_.size(); i++) {
                        tree_widths_[i] = static_cast<u8>((tree_widths_[i] + tree_widths_[i - 1]) % tree_upper_width_);
                    }
                }
                // Add lower_width offset
                for (auto& w : tree_widths_) {
                    if (w > 0) {
                        w = static_cast<u8>(w + tree_lower_width_);
                    }
                }
                // Don't pad - use the exact number of widths read
                auto r = build_huffman_tree(tree_widths_, MAXCODEWIDTH, len_tree_);
                if (!r) return crate::make_unexpected(r.error());
                state_ = state::READ_SYMS_COUNT;
                break;
            }

            case state::READ_SYMS_COUNT: {
                u32 count = 0;
                if (!try_read_bits(in_ptr, in_end, 15, count)) {
                    return finalize(decode_status::needs_more_input);
                }
                syms_to_read_ = static_cast<int>(count);
                if (g_debug) {
                    std::cerr << "SYMS_COUNT: " << count
                              << " buffer=0x" << std::hex << bit_buffer_ << std::dec
                              << " bits_left=" << bits_left_ << std::endl;
                }
                state_ = state::READ_MAIN_SYMBOL;
                break;
            }

            case state::READ_MAIN_SYMBOL: {
                // Check if we need to re-read trees
                if (syms_to_read_ == 0) {
                    state_ = state::READ_MAIN_TREE_HEADER;
                    break;
                }

                static bool first_main_sym_debug = true;
                if (g_debug && first_main_sym_debug && window_.size() == 0) {
                    // Calculate what peek(11) would return
                    u32 peek_val = static_cast<u32>(bit_buffer_ >> (64 - 11));
                    std::cerr << "BEFORE first main sym: buffer=0x" << std::hex << bit_buffer_
                              << std::dec << " bits_left=" << bits_left_
                              << " peek(11)=" << peek_val << std::endl;
                    first_main_sym_debug = false;
                }

                u16 symbol = 0;
                if (!try_read_huffman_symbol(in_ptr, in_end, main_tree_, symbol)) {
                    if (input_finished) {
                        state_ = state::DONE;
                        return finalize(decode_status::done);
                    }
                    return finalize(decode_status::needs_more_input);
                }
                syms_to_read_--;
                current_symbol_ = symbol;

                static int main_sym_debug = 30;
                if (g_debug && main_sym_debug > 0) {
                    std::cerr << "MAIN_SYM: " << symbol
                              << (symbol <= 255 ? " (lit)" : symbol <= 259 ? " (hist)" : " (dist)")
                              << " window=" << window_.size() << std::endl;
                    main_sym_debug--;
                }

                if (symbol <= 255) {
                    state_ = state::OUTPUT_LITERAL;
                } else if (symbol < TYPECODE) {
                    if (symbol <= 259) {
                        // Distance from history
                        state_ = state::READ_LEN_SYMBOL_HIST;
                    } else {
                        // Distance from bitstream
                        state_ = state::READ_DIST_BITS;
                    }
                } else if (symbol == TYPECODE) {
                    state_ = state::READ_MODE_TYPE;
                } else {
                    return crate::make_unexpected(error{error_code::CorruptData, "ACE LZ77 invalid symbol"});
                }
                break;
            }

            case state::OUTPUT_LITERAL: {
                if (out_ptr >= out_end) {
                    return finalize(decode_status::needs_more_output);
                }
                output_byte(static_cast<u8>(current_symbol_));
                state_ = state::READ_MAIN_SYMBOL;
                break;
            }

            case state::READ_LEN_SYMBOL_HIST: {
                u16 len_sym = 0;
                if (!try_read_huffman_symbol(in_ptr, in_end, len_tree_, len_sym)) {
                    return finalize(decode_status::needs_more_input);
                }

                int offset = current_symbol_ & 0x03;
                // Retrieve from history (pop and push to end)
                size_t idx = static_cast<size_t>(4 - offset - 1);
                copy_dist_ = dist_hist_[idx];
                for (size_t i = idx; i < 3; i++) {
                    dist_hist_[i] = dist_hist_[i + 1];
                }
                dist_hist_[3] = copy_dist_;

                copy_len_ = len_sym;
                if (offset > 1) {
                    copy_len_ += 3;
                } else {
                    copy_len_ += 2;
                }
                copy_dist_ += 1;
                copy_pos_ = 0;
                if (g_debug) {
                    std::cerr << "Copy(hist): dist=" << copy_dist_ << " len=" << copy_len_
                              << " window=" << window_.size() << " offset=" << offset << std::endl;
                }
                state_ = state::COPY_MATCH;
                break;
            }

            case state::READ_DIST_BITS: {
                int dist_bits = current_symbol_ - 260;
                u32 dist_val = 0;

                if (dist_bits < 2) {
                    dist_val = static_cast<u32>(dist_bits);
                } else {
                    u32 extra = 0;
                    if (!try_read_bits(in_ptr, in_end, dist_bits - 1, extra)) {
                        return finalize(decode_status::needs_more_input);
                    }
                    dist_val = extra + (1u << (dist_bits - 1));
                }

                if (g_debug) {
                    std::cerr << "READ_DIST: symbol=" << current_symbol_
                              << " dist_bits=" << dist_bits
                              << " dist_val=" << dist_val << std::endl;
                }

                copy_dist_ = dist_val;
                // Append to history
                for (size_t i = 0; i < 3; i++) {
                    dist_hist_[i] = dist_hist_[i + 1];
                }
                dist_hist_[3] = copy_dist_;

                state_ = state::READ_LEN_SYMBOL_NEW;
                break;
            }

            case state::READ_LEN_SYMBOL_NEW: {
                u16 len_sym = 0;
                if (!try_read_huffman_symbol(in_ptr, in_end, len_tree_, len_sym)) {
                    return finalize(decode_status::needs_more_input);
                }

                copy_len_ = len_sym;
                if (copy_dist_ <= MAXDISTATLEN2) {
                    copy_len_ += 2;
                } else if (copy_dist_ <= MAXDISTATLEN3) {
                    copy_len_ += 3;
                } else {
                    copy_len_ += 4;
                }
                copy_dist_ += 1;
                copy_pos_ = 0;
                if (g_debug) {
                    std::cerr << "Copy(new): dist=" << copy_dist_ << " len=" << copy_len_
                              << " window=" << window_.size() << std::endl;
                }
                state_ = state::COPY_MATCH;
                break;
            }

            case state::COPY_MATCH: {
                while (copy_pos_ < copy_len_) {
                    if (out_ptr >= out_end) {
                        return finalize(decode_status::needs_more_output);
                    }

                    if (copy_dist_ > window_.size()) {
                        if (g_debug) {
                            std::cerr << "COPY_MATCH error: dist=" << copy_dist_
                                      << " window.size=" << window_.size()
                                      << " len=" << copy_len_ << " pos=" << copy_pos_ << std::endl;
                        }
                        return crate::make_unexpected(error{error_code::CorruptData, "ACE LZ77 copy source out of bounds"});
                    }

                    // Source index: window_.size() already accounts for bytes output so far
                    // in this copy operation, so no need to add copy_pos_
                    size_t src_idx = window_.size() - copy_dist_;
                    u8 b = window_[src_idx];
                    output_byte(b);
                    copy_pos_++;
                }
                state_ = state::READ_MAIN_SYMBOL;
                break;
            }

            case state::READ_MODE_TYPE: {
                u32 mode = 0;
                if (!try_read_bits(in_ptr, in_end, 8, mode)) {
                    return finalize(decode_status::needs_more_input);
                }
                if (mode == 1) {
                    state_ = state::READ_MODE_DELTA;
                } else if (mode == 2) {
                    state_ = state::READ_MODE_EXE;
                } else {
                    state_ = state::READ_MAIN_SYMBOL;
                }
                break;
            }

            case state::READ_MODE_DELTA: {
                u32 delta_dist = 0, delta_len = 0;
                if (!try_read_bits(in_ptr, in_end, 8, delta_dist)) {
                    return finalize(decode_status::needs_more_input);
                }
                if (!try_read_bits(in_ptr, in_end, 17, delta_len)) {
                    return finalize(decode_status::needs_more_input);
                }
                // Ignore mode info, continue with LZ77
                state_ = state::READ_MAIN_SYMBOL;
                break;
            }

            case state::READ_MODE_EXE: {
                u32 exe_mode = 0;
                if (!try_read_bits(in_ptr, in_end, 8, exe_mode)) {
                    return finalize(decode_status::needs_more_input);
                }
                // Ignore mode info, continue with LZ77
                state_ = state::READ_MAIN_SYMBOL;
                break;
            }

            case state::DONE:
                break;
        }
    }

    return finalize(decode_status::done);
}

}  // namespace crate
