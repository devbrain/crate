#include <crate/formats/ace.hh>
#include <cstring>
#include <crate/core/crc.hh>

namespace crate {


    namespace ace {
        // =============================================================================
        // BitStream - Intel-endian 32bit-byte-swapped, MSB first bitstream
        // =============================================================================
        class bit_stream {
            public:
                static result_t <bit_stream> create(byte_span data) {
                    bit_stream bs(data);
                    auto r = bs.refill();
                    if (!r)
                        return std::unexpected(r.error());
                    return bs;
                }

                result_t <u32> peek_bits(int bits) {
                    // Limited to 31 bits max (like C implementation in acefile)
                    if (bits > 31)
                        bits = 31;

                    if (pos_ + bits > len_) {
                        auto r = refill();
                        if (!r) {
                            // Allow peeking up to 31 bits beyond EOF (as zeros)
                            if (!buf_.empty()) {
                                buf_.push_back(0);
                                len_ += 32;
                            } else {
                                return std::unexpected(r.error());
                            }
                        }
                    }

                    int peeked = std::min(bits, 32 - (pos_ % 32));
                    size_t index = static_cast <size_t>(pos_ / 32);
                    if (index >= buf_.size()) {
                        return 0u;
                    }
                    u32 res = get_bits(buf_[index], pos_ % 32, peeked);

                    if (bits - peeked > 0) {
                        res <<= (bits - peeked);
                        size_t next_index = static_cast <size_t>((pos_ + peeked) / 32);
                        if (next_index >= buf_.size()) {
                            return res;
                        }
                        res += get_bits(buf_[next_index], 0, bits - peeked);
                    }

                    return res;
                }

                void_result_t skip_bits(int bits) {
                    if (pos_ + bits > len_) {
                        auto r = refill();
                        if (!r)
                            return std::unexpected(r.error());
                    }
                    pos_ += bits;
                    return {};
                }

                result_t <u32> read_bits(int bits) {
                    auto value = peek_bits(bits);
                    if (!value)
                        return std::unexpected(value.error());
                    auto r = skip_bits(bits);
                    if (!r)
                        return std::unexpected(r.error());
                    return *value;
                }

                // Read unsigned int with known bit width (MSB is implicit 1)
                result_t <u32> read_known_width_uint(int bits) {
                    if (bits < 2)
                        return static_cast <u32>(bits);
                    bits -= 1;
                    auto v = read_bits(bits);
                    if (!v)
                        return std::unexpected(v.error());
                    return *v + (1u << bits);
                }

                [[nodiscard]] bool eof() const { return data_pos_ >= data_.size() && pos_ >= len_; }

            private:
                explicit bit_stream(byte_span data)
                    : data_(data) {
                }

                static u32 get_bits(u32 value, int start, int length) {
                    u32 mask = ((0xFFFFFFFFu << (32 - length)) & 0xFFFFFFFFu) >> start;
                    return (value & mask) >> (32 - length - start);
                }

                void_result_t refill() {
                    // Keep last 32-bit word if we have one
                    std::vector <u32> new_buf;
                    if (!buf_.empty() && buf_idx_ < buf_.size()) {
                        new_buf.push_back(buf_.back());
                    }

                    // Read more data in 4-byte chunks
                    while (data_pos_ + 4 <= data_.size() && new_buf.size() < 1024) {
                        u32 word = static_cast <u32>(data_[data_pos_]) | (static_cast <u32>(data_[data_pos_ + 1]) << 8)
                                   |
                                   (static_cast <u32>(data_[data_pos_ + 2]) << 16) | (
                                       static_cast <u32>(data_[data_pos_ + 3]) << 24);
                        new_buf.push_back(word);
                        data_pos_ += 4;
                    }

                    if (new_buf.empty() && buf_.empty()) {
                        return std::unexpected(error{error_code::InputBufferUnderflow, "Cannot refill beyond EOF"});
                    }

                    if (pos_ > 0 && !buf_.empty()) {
                        pos_ -= (len_ - 32);
                    }

                    buf_ = std::move(new_buf);
                    buf_idx_ = 0;
                    len_ = 32 * static_cast <int>(buf_.size());
                    return {};
                }

                byte_span data_{};
                size_t data_pos_ = 0;
                std::vector <u32> buf_;
                size_t buf_idx_ = 0;
                int len_ = 0;
                int pos_ = 0;
        };

        // =============================================================================
        // Huffman tree decoder
        // =============================================================================
        class huffman {
            public:
                struct tree {
                    std::vector <u16> codes; // symbol lookup table
                    std::vector <u8> widths; // bit width per symbol
                    int max_width = 0;

                    result_t <u16> read_symbol(bit_stream& bs) const {
                        auto peek_result = bs.peek_bits(max_width);
                        if (!peek_result)
                            return std::unexpected(peek_result.error());
                        u32 maxwidth_code = *peek_result;
                        if (maxwidth_code >= codes.size()) {
                            return std::unexpected(error{
                                error_code::CorruptData, "Huffman: maxwidth_code >= codes.size()"
                            });
                        }
                        u16 symbol = codes[maxwidth_code];
                        auto skip_result = bs.skip_bits(widths[symbol]);
                        if (!skip_result)
                            return std::unexpected(skip_result.error());
                        return symbol;
                    }
                };

                static constexpr int WIDTHWIDTHBITS = 3;
                static constexpr int MAXWIDTHWIDTH = (1 << WIDTHWIDTHBITS) - 1;

                static result_t <tree> make_tree(std::vector <u8>& widths, int max_width) {
                    // Sort symbols by width (descending) using unstable quicksort
                    std::vector <int> sorted_symbols(widths.size());
                    std::vector <int> sorted_widths(widths.size());
                    for (size_t i = 0; i < widths.size(); i++) {
                        sorted_symbols[i] = static_cast <int>(i);
                        sorted_widths[i] = widths[i];
                    }

                    quicksort(sorted_widths, sorted_symbols, 0, static_cast <int>(sorted_widths.size()) - 1);

                    // Count used (non-zero width) symbols
                    int used = 0;
                    while (used < static_cast <int>(sorted_widths.size()) && sorted_widths[static_cast <size_t>(used)]
                           != 0) {
                        used++;
                    }

                    // Handle edge cases
                    if (used < 2) {
                        widths[static_cast <size_t>(sorted_symbols[0])] = 1;
                        if (used == 0)
                            used = 1;
                    }

                    sorted_symbols.resize(static_cast <size_t>(used));
                    sorted_widths.resize(static_cast <size_t>(used));

                    // Build codes table
                    tree t;
                    t.max_width = max_width;
                    t.widths.assign(widths.begin(), widths.end());

                    size_t max_codes = 1u << max_width;
                    t.codes.reserve(max_codes);

                    for (int i = used - 1; i >= 0; i--) {
                        int sym = sorted_symbols[static_cast <size_t>(i)];
                        int wdt = sorted_widths[static_cast <size_t>(i)];
                        if (wdt > max_width) {
                            return std::unexpected(error{error_code::CorruptData, "Huffman: width > max_width"});
                        }
                        size_t repeat = 1u << (max_width - wdt);
                        for (size_t j = 0; j < repeat; j++) {
                            t.codes.push_back(static_cast <u16>(sym));
                        }
                        if (t.codes.size() > max_codes) {
                            return std::unexpected(error{error_code::CorruptData, "Huffman: codes > max_codes"});
                        }
                    }

                    return t;
                }

                static result_t <tree> read_tree(bit_stream& bs, int max_width, int num_codes) {
                    auto num_widths_r = bs.read_bits(9);
                    if (!num_widths_r)
                        return std::unexpected(num_widths_r.error());
                    int num_widths = static_cast <int>(*num_widths_r) + 1;
                    if (num_widths > num_codes + 1) {
                        num_widths = num_codes + 1;
                    }

                    auto lower_width_r = bs.read_bits(4);
                    if (!lower_width_r)
                        return std::unexpected(lower_width_r.error());
                    int lower_width = static_cast <int>(*lower_width_r);

                    auto upper_width_r = bs.read_bits(4);
                    if (!upper_width_r)
                        return std::unexpected(upper_width_r.error());
                    int upper_width = static_cast <int>(*upper_width_r);

                    // Read width widths
                    std::vector <u8> width_widths;
                    int width_num_widths = upper_width + 1;
                    for (int i = 0; i < width_num_widths; i++) {
                        auto bits_r = bs.read_bits(WIDTHWIDTHBITS);
                        if (!bits_r)
                            return std::unexpected(bits_r.error());
                        width_widths.push_back(static_cast <u8>(*bits_r));
                    }

                    auto width_tree_r = make_tree(width_widths, MAXWIDTHWIDTH);
                    if (!width_tree_r)
                        return std::unexpected(width_tree_r.error());
                    tree width_tree = std::move(*width_tree_r);

                    // Read widths
                    std::vector <u8> widths;
                    while (static_cast <int>(widths.size()) < num_widths) {
                        auto symbol_r = width_tree.read_symbol(bs);
                        if (!symbol_r)
                            return std::unexpected(symbol_r.error());
                        u16 symbol = *symbol_r;
                        if (static_cast <int>(symbol) < upper_width) {
                            widths.push_back(static_cast <u8>(symbol));
                        } else {
                            auto len_r = bs.read_bits(4);
                            if (!len_r)
                                return std::unexpected(len_r.error());
                            int length = static_cast <int>(*len_r) + 4;
                            length = std::min(length, num_widths - static_cast <int>(widths.size()));
                            for (int i = 0; i < length; i++) {
                                widths.push_back(0);
                            }
                        }
                    }

                    // Delta decode widths
                    if (upper_width > 0) {
                        for (size_t i = 1; i < widths.size(); i++) {
                            widths[i] = static_cast <u8>((widths[i] + widths[i - 1]) % upper_width);
                        }
                    }

                    // Add lower_width offset
                    for (auto& w : widths) {
                        if (w > 0) {
                            w = static_cast <u8>(w + lower_width);
                        }
                    }

                    return make_tree(widths, max_width);
                }

            private:
                static void quicksort(std::vector <int>& keys, std::vector <int>& values, int left, int right) {
                    if (left >= right)
                        return;

                    int new_left = left;
                    int new_right = right;
                    int m = keys[static_cast <size_t>(right)];

                    while (true) {
                        while (keys[static_cast <size_t>(new_left)] > m)
                            new_left++;
                        while (keys[static_cast <size_t>(new_right)] < m)
                            new_right--;

                        if (new_left <= new_right) {
                            std::swap(keys[static_cast <size_t>(new_left)], keys[static_cast <size_t>(new_right)]);
                            std::swap(values[static_cast <size_t>(new_left)], values[static_cast <size_t>(new_right)]);
                            new_left++;
                            new_right--;
                        }

                        if (new_left >= new_right)
                            break;
                    }

                    if (left < new_right) {
                        if (left < new_right - 1) {
                            quicksort(keys, values, left, new_right);
                        } else if (keys[static_cast <size_t>(left)] < keys[static_cast <size_t>(new_right)]) {
                            std::swap(keys[static_cast <size_t>(left)], keys[static_cast <size_t>(new_right)]);
                            std::swap(values[static_cast <size_t>(left)], values[static_cast <size_t>(new_right)]);
                        }
                    }

                    if (right > new_left) {
                        if (new_left < right - 1) {
                            quicksort(keys, values, new_left, right);
                        } else if (keys[static_cast <size_t>(new_left)] < keys[static_cast <size_t>(right)]) {
                            std::swap(keys[static_cast <size_t>(new_left)], keys[static_cast <size_t>(right)]);
                            std::swap(values[static_cast <size_t>(new_left)], values[static_cast <size_t>(right)]);
                        }
                    }
                }
        };

        // =============================================================================
        // LZ77 Decompression
        // =============================================================================
        class lz77_decoder {
            public:
                // LZ77 constants
                static constexpr int MAXCODEWIDTH = 11;
                static constexpr int MAXLEN = 259;
                static constexpr int MAXDISTATLEN2 = 255;
                static constexpr int MAXDISTATLEN3 = 8191;
                static constexpr int MINDICBITS = 10;
                static constexpr int MAXDICBITS = 22;
                static constexpr size_t MINDICSIZE = 1 << MINDICBITS;
                static constexpr size_t MAXDICSIZE = 1 << MAXDICBITS;
                static constexpr int TYPECODE = 260 + MAXDICBITS + 1; // 283
                static constexpr int NUMMAINCODES = 260 + MAXDICBITS + 2; // 284
                static constexpr int NUMLENCODES = 256 - 1; // 255

                lz77_decoder()
                    : dic_size_(MINDICSIZE) {
                }

                void set_dic_size(size_t size) { dic_size_ = std::min(std::max(size, MINDICSIZE), MAXDICSIZE); }

                result_t <byte_vector> decompress(byte_span data, size_t orig_size) {
                    auto bs_result = bit_stream::create(data);
                    if (!bs_result)
                        return std::unexpected(bs_result.error());
                    bit_stream bs = std::move(*bs_result);

                    byte_vector output;
                    output.reserve(orig_size);

                    // Initialize distance history
                    std::array <u32, 4> dist_hist = {0, 0, 0, 0};

                    // Read initial Huffman trees
                    int syms_to_read = 0;
                    huffman::tree main_tree;
                    huffman::tree len_tree;

                    // Helper macros to propagate errors
#define TRY(expr)                               \
    do {                                        \
        auto _r = (expr);                       \
        if (!_r)                                \
            return std::unexpected(_r.error()); \
    } while (0)
#define TRY_VAL(var, expr)                       \
    auto var##_r = (expr);                       \
    if (!var##_r)                                \
        return std::unexpected(var##_r.error()); \
    auto var = *var##_r

                    auto read_trees = [&]() -> void_result_t {
                        auto main_r = huffman::read_tree(bs, MAXCODEWIDTH, NUMMAINCODES);
                        if (!main_r)
                            return std::unexpected(main_r.error());
                        main_tree = std::move(*main_r);

                        auto len_r = huffman::read_tree(bs, MAXCODEWIDTH, NUMLENCODES);
                        if (!len_r)
                            return std::unexpected(len_r.error());
                        len_tree = std::move(*len_r);

                        auto syms_r = bs.read_bits(15);
                        if (!syms_r)
                            return std::unexpected(syms_r.error());
                        syms_to_read = static_cast <int>(*syms_r);
                        return {};
                    };

                    auto read_main_symbol = [&]() -> result_t <u16> {
                        if (syms_to_read == 0) {
                            auto r = read_trees();
                            if (!r)
                                return std::unexpected(r.error());
                        }
                        syms_to_read--;
                        return main_tree.read_symbol(bs);
                    };

                    auto read_len_symbol = [&]() -> result_t <u16> { return len_tree.read_symbol(bs); };

                    auto dist_hist_retrieve = [&](int offset) -> u32 {
                        // Pop from position (SIZE - offset - 1), append to end
                        int idx = 4 - offset - 1;
                        size_t idx_pos = static_cast <size_t>(idx);
                        u32 dist = dist_hist[idx_pos];
                        for (size_t i = idx_pos; i < 3; i++) {
                            dist_hist[i] = dist_hist[i + 1];
                        }
                        dist_hist[3] = dist;
                        return dist;
                    };

                    auto dist_hist_append = [&](u32 dist) {
                        for (size_t i = 0; i < 3; i++) {
                            dist_hist[i] = dist_hist[i + 1];
                        }
                        dist_hist[3] = dist;
                    };

                    while (output.size() < orig_size) {
                        TRY_VAL(symbol, read_main_symbol());

                        if (symbol <= 255) {
                            // Literal byte
                            output.push_back(static_cast <u8>(symbol));
                        } else if (symbol < TYPECODE) {
                            // Copy from dictionary
                            u32 copy_dist;
                            int copy_len;

                            if (symbol <= 259) {
                                // Distance from history
                                int offset = symbol & 0x03;
                                copy_dist = dist_hist_retrieve(offset);
                                TRY_VAL(len_sym, read_len_symbol());
                                copy_len = static_cast <int>(len_sym);
                                if (offset > 1) {
                                    copy_len += 3;
                                } else {
                                    copy_len += 2;
                                }
                            } else {
                                // Distance from bitstream
                                int dist_bits = symbol - 260;
                                TRY_VAL(dist_val, bs.read_known_width_uint(dist_bits));
                                copy_dist = dist_val;
                                TRY_VAL(len_sym, read_len_symbol());
                                copy_len = static_cast <int>(len_sym);
                                dist_hist_append(copy_dist);

                                if (copy_dist <= MAXDISTATLEN2) {
                                    copy_len += 2;
                                } else if (copy_dist <= MAXDISTATLEN3) {
                                    copy_len += 3;
                                } else {
                                    copy_len += 4;
                                }
                            }

                            copy_dist += 1;

                            // Copy from dictionary
                            if (copy_dist > output.size()) {
                                return std::unexpected(error(error_code::CorruptData,
                                                             "LZ77 copy source out of bounds"));
                            }

                            size_t src_pos = output.size() - copy_dist;
                            size_t copy_len_size = static_cast <size_t>(copy_len);
                            for (size_t i = 0; i < copy_len_size && output.size() < orig_size; i++) {
                                output.push_back(output[src_pos + i]);
                            }
                        } else if (symbol == TYPECODE) {
                            // Mode switch (ACE 2.0 blocked format)
                            // For now, we only support basic LZ77 mode
                            // Read and ignore mode info
                            TRY_VAL(mode, bs.read_bits(8));
                            if (mode == 1) {
                                // LZ77_DELTA: delta_dist (8 bits), delta_len (17 bits)
                                TRY(bs.read_bits(8));
                                TRY(bs.read_bits(17));
                            } else if (mode == 2) {
                                // LZ77_EXE: exe_mode (8 bits)
                                TRY(bs.read_bits(8));
                            }
                            // Continue with LZ77 decompression
                        } else {
                            return std::unexpected(error(error_code::CorruptData, "LZ77 invalid symbol"));
                        }
                    }

#undef TRY
#undef TRY_VAL

                    return output;
                }

            private:
                size_t dic_size_;
        };

        constexpr u8 SIGNATURE[7] = {'*', '*', 'A', 'C', 'E', '*', '*'};
        constexpr size_t SIG_OFFSET = 7; // Signature is at offset 7 in main header

        enum header_type : u8 { MAIN_HEADER = 0, FILE_HEADER = 1, RECOVERY_RECORD = 2 };

        enum compression : u8 {
            STORED = 0,
            LZ77_V1 = 1, // ACE 1.0 LZ77
            LZ77_V2 = 2 // ACE 2.0 blocked LZ77
        };

        enum quality : u8 { FASTEST = 0, FAST = 1, NORMAL = 2, GOOD = 3, BEST = 4 };

        // Header flags (common)
        constexpr u16 FLAG_ADDSIZE = 0x0001; // ADDSIZE field present
        constexpr u16 FLAG_COMMENT = 0x0002; // Comment present (main) / passwd (file)

        // Main header flags
        constexpr u16 FLAG_SFX = 0x0200; // Self-extracting archive
        constexpr u16 FLAG_MULTIVOLUME = 0x0800; // Multi-volume archive
        constexpr u16 FLAG_AV_STRING = 0x1000; // AV string present
        constexpr u16 FLAG_RECOVERY = 0x2000; // Recovery record present
        constexpr u16 FLAG_LOCKED = 0x4000; // Archive is locked
        constexpr u16 FLAG_SOLID = 0x8000; // Solid archive

        // File header flags
        constexpr u16 FLAG_ENCRYPTED = 0x0004; // File is encrypted
        constexpr u16 FLAG_CONTINUED_PREV = 0x0008; // Continued from previous volume
        constexpr u16 FLAG_CONTINUED_NEXT = 0x0010; // Continued in next volume
        constexpr u16 FLAG_SOLID_FILE = 0x8000; // Solid file (depends on previous)

        struct main_header {
            u16 crc = 0;
            u16 size = 0;
            u8 type = 0;
            u16 flags = 0;
            u8 ver_extract = 0;
            u8 ver_created = 0;
            u8 host_created = 0;
            u8 volume_num = 0;
            u32 creation_time = 0;
            std::string av_string;
            std::string comment;
        };

        struct file_header {
            u16 crc = 0;
            u16 size = 0;
            u8 type = 0;
            u16 flags = 0;
            u32 pack_size = 0;
            u32 orig_size = 0;
            dos_date_time datetime{};
            u32 attributes = 0;
            u32 file_crc = 0;
            u8 compression = 0;
            u8 quality = 0;
            u16 params = 0;
            std::string name;
        };

        // ACE CRC-32: standard polynomial but NO final inversion
        // Uses core crc_32::value() which returns the unfinalised CRC
        inline u32 ace_crc32(byte_span data) {
            crc_32 crc;
            crc.update(data);
            return crc.value(); // ACE uses unfinalised CRC (no final XOR)
        }

        // CRC-16 for ACE headers (lower 16 bits of ACE CRC-32)
        inline u16 ace_crc16(byte_span data) {
            return static_cast <u16>(ace_crc32(data) & 0xFFFF);
        }
    } // namespace ace


    struct ace_archive::impl {
        byte_vector data_;
        ace::main_header main_header_;
        std::vector <ace::file_header> members_;
        std::vector <file_entry> files_;
    };

    ace_archive::ace_archive()
        : m_pimpl(std::make_unique<impl>()) {

    }

    ace_archive::~ace_archive() = default;

    result_t <std::unique_ptr <ace_archive>> ace_archive::open(byte_span data) {
        auto archive = std::unique_ptr <ace_archive>(new ace_archive());
        archive->m_pimpl->data_.assign(data.begin(), data.end());

        auto result = archive->parse();
        if (!result)
            return std::unexpected(result.error());

        return archive;
    }

    result_t <std::unique_ptr <ace_archive>> ace_archive::open(const std::filesystem::path& path) {
        auto file = file_input_stream::open(path);
        if (!file)
            return std::unexpected(file.error());

        auto size = file->size();
        if (!size)
            return std::unexpected(size.error());

        byte_vector data(*size);
        auto read = file->read(data);
        if (!read)
            return std::unexpected(read.error());

        return open(data);
    }

    const std::vector <file_entry>& ace_archive::files() const {
        return m_pimpl->files_;
    }

    result_t <byte_vector> ace_archive::extract(const file_entry& entry) {
        if (entry.folder_index >= m_pimpl->members_.size()) {
            return std::unexpected(error{error_code::FileNotInArchive});
        }

        const auto& member = m_pimpl->members_[entry.folder_index];

        // Check if solid archive - requires sequential decompression
        // For solid archives, we can only extract if this is the first file
        // (or we've extracted all previous files in order)
        if ((m_pimpl->main_header_.flags & ace::FLAG_SOLID) && entry.folder_index > 0) {
            // Check if this file depends on previous files (SOLID flag per file)
            if (member.flags & ace::FLAG_SOLID_FILE) {
                return std::unexpected(
                    error{error_code::UnsupportedCompression, "ACE solid archives require sequential decompression"});
            }
        }

        // Check if file is encrypted
        if (member.flags & ace::FLAG_ENCRYPTED) {
            return std::unexpected(error{error_code::UnsupportedCompression, "ACE encrypted files not supported"});
        }

        // Check if file spans volumes
        if (member.flags & (ace::FLAG_CONTINUED_PREV | ace::FLAG_CONTINUED_NEXT)) {
            return std::unexpected(error{error_code::UnsupportedCompression, "ACE multi-volume files not supported"});
        }

        if (entry.folder_offset + member.pack_size > m_pimpl->data_.size()) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        byte_span compressed(m_pimpl->data_.data() + entry.folder_offset, member.pack_size);
        byte_vector output;

        switch (member.compression) {
            case ace::STORED:
                output.assign(compressed.begin(), compressed.end());
                break;

            case ace::LZ77_V1:
            case ace::LZ77_V2: {
                ace::lz77_decoder decoder;
                // Set dictionary size from params if available
                if (member.params > 0) {
                    size_t dic_bits = 10 + member.params;
                    decoder.set_dic_size(1u << dic_bits);
                }
                auto result = decoder.decompress(compressed, member.orig_size);
                if (!result)
                    return std::unexpected(result.error());
                output = std::move(*result);
                break;
            }

            default:
                return std::unexpected(error{error_code::UnsupportedCompression, "Unknown ACE compression method"});
        }

        // Verify ACE CRC-32 (different from standard CRC-32)
        u32 calc_crc = ace::ace_crc32(output);
        if (calc_crc != member.file_crc) {
            return std::unexpected(error{
                error_code::InvalidChecksum,
                "CRC-32 mismatch (expected 0x" + std::to_string(member.file_crc) + ", got 0x" +
                std::to_string(calc_crc) + ", size=" + std::to_string(output.size()) + ")"
            });
        }

        return output;
    }

    void_result_t ace_archive::parse() {
        // Find signature "**ACE**"
        size_t sig_pos = find_signature();
        if (sig_pos == std::string::npos) {
            return std::unexpected(error{error_code::InvalidSignature, "ACE signature not found"});
        }

        // Main header starts at sig_pos - 7
        if (sig_pos < ace::SIG_OFFSET) {
            return std::unexpected(error{error_code::InvalidSignature, "Invalid ACE header position"});
        }

        size_t pos = sig_pos - ace::SIG_OFFSET;

        // Parse main header
        auto main_result = parse_main_header(pos);
        if (!main_result)
            return std::unexpected(main_result.error());

        // Parse file headers
        while (pos < m_pimpl->data_.size()) {
            // Check if we have enough for block header
            if (pos + 7 > m_pimpl->data_.size())
                break;

            u16 crc = read_u16_le(m_pimpl->data_.data() + pos);
            u16 size = read_u16_le(m_pimpl->data_.data() + pos + 2);
            u8 type = m_pimpl->data_[pos + 4];
            u16 flags = read_u16_le(m_pimpl->data_.data() + pos + 5);

            if (size == 0)
                break;

            // Verify CRC of header
            if (pos + 4 + size > m_pimpl->data_.size())
                break;
            byte_span header_data(m_pimpl->data_.data() + pos + 4, size);
            u16 calc_crc = ace::ace_crc16(header_data);
            if (calc_crc != crc) {
                // CRC mismatch, might be end of archive
                break;
            }

            if (type == ace::FILE_HEADER) {
                auto file_result = parse_file_header(pos);
                if (!file_result)
                    break;
            } else if (type == ace::RECOVERY_RECORD) {
                // Skip recovery record
                size_t block_size = 4 + size;
                if (flags & ace::FLAG_ADDSIZE) {
                    if (pos + 7 + 4 > m_pimpl->data_.size())
                        break;
                    u32 addsize = read_u32_le(m_pimpl->data_.data() + pos + 7);
                    block_size += addsize;
                }
                pos += block_size;
            } else {
                break;
            }
        }

        return {};
    }

    size_t ace_archive::find_signature() const {
        for (size_t i = ace::SIG_OFFSET; i + 7 <= m_pimpl->data_.size(); i++) {
            if (std::memcmp(m_pimpl->data_.data() + i, ace::SIGNATURE, 7) == 0) {
                return i;
            }
        }
        return std::string::npos;
    }

    void_result_t ace_archive::parse_main_header(size_t& pos) {
        if (pos + 7 > m_pimpl->data_.size()) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        m_pimpl->main_header_.crc = read_u16_le(m_pimpl->data_.data() + pos);
        m_pimpl->main_header_.size = read_u16_le(m_pimpl->data_.data() + pos + 2);
        m_pimpl->main_header_.type = m_pimpl->data_[pos + 4];
        m_pimpl->main_header_.flags = read_u16_le(m_pimpl->data_.data() + pos + 5);

        if (m_pimpl->main_header_.type != ace::MAIN_HEADER) {
            return std::unexpected(error{error_code::InvalidSignature, "Expected ACE main header"});
        }

        size_t header_end = pos + 4 + m_pimpl->main_header_.size;
        if (header_end > m_pimpl->data_.size()) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        // Skip signature (7 bytes)
        size_t field_pos = pos + 7 + 7;

        if (field_pos + 8 > header_end) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        m_pimpl->main_header_.ver_extract = m_pimpl->data_[field_pos++];
        m_pimpl->main_header_.ver_created = m_pimpl->data_[field_pos++];
        m_pimpl->main_header_.host_created = m_pimpl->data_[field_pos++];
        m_pimpl->main_header_.volume_num = m_pimpl->data_[field_pos++];
        m_pimpl->main_header_.creation_time = read_u32_le(m_pimpl->data_.data() + field_pos);
        field_pos += 4;

        // Skip reserved bytes (8 bytes in ACE 2.0)
        field_pos += 8;

        // AV string if present
        if (m_pimpl->main_header_.flags & ace::FLAG_AV_STRING) {
            if (field_pos >= header_end) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }
            u8 av_len = m_pimpl->data_[field_pos++];
            if (field_pos + av_len > header_end) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }
            m_pimpl->main_header_.av_string.assign(reinterpret_cast <const char*>(m_pimpl->data_.data() + field_pos), av_len);
            field_pos += av_len;
        }

        // Comment if present
        if (m_pimpl->main_header_.flags & ace::FLAG_COMMENT) {
            if (field_pos + 2 > header_end) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }
            u16 comment_len = read_u16_le(m_pimpl->data_.data() + field_pos);
            field_pos += 2;
            if (field_pos + comment_len > header_end) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }
            m_pimpl->main_header_.comment.assign(reinterpret_cast <const char*>(m_pimpl->data_.data() + field_pos), comment_len);
            field_pos += comment_len;
        }

        pos = header_end;
        return {};
    }

    void_result_t ace_archive::parse_file_header(size_t& pos) {
        if (pos + 7 > m_pimpl->data_.size()) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        ace::file_header member;
        member.crc = read_u16_le(m_pimpl->data_.data() + pos);
        member.size = read_u16_le(m_pimpl->data_.data() + pos + 2);
        member.type = m_pimpl->data_[pos + 4];
        member.flags = read_u16_le(m_pimpl->data_.data() + pos + 5);

        size_t header_end = pos + 4 + member.size;
        if (header_end > m_pimpl->data_.size()) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        size_t field_pos = pos + 7;

        if (field_pos + 20 > header_end) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        member.pack_size = read_u32_le(m_pimpl->data_.data() + field_pos);
        field_pos += 4;
        member.orig_size = read_u32_le(m_pimpl->data_.data() + field_pos);
        field_pos += 4;

        u32 ftime = read_u32_le(m_pimpl->data_.data() + field_pos);
        member.datetime.time = static_cast <u16>(ftime & 0xFFFF);
        member.datetime.date = static_cast <u16>((ftime >> 16) & 0xFFFF);
        field_pos += 4;

        member.attributes = read_u32_le(m_pimpl->data_.data() + field_pos);
        field_pos += 4;
        member.file_crc = read_u32_le(m_pimpl->data_.data() + field_pos);
        field_pos += 4;

        if (field_pos + 4 > header_end) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        // TECH info - compression type and quality
        u32 tech = read_u32_le(m_pimpl->data_.data() + field_pos);
        member.compression = static_cast <u8>(tech & 0xFF);
        member.quality = static_cast <u8>((tech >> 8) & 0xFF);
        member.params = static_cast <u16>((tech >> 16) & 0xFFFF);
        field_pos += 4;

        // Reserved (2 bytes)
        field_pos += 2;

        if (field_pos + 2 > header_end) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        // Filename
        u16 fname_len = read_u16_le(m_pimpl->data_.data() + field_pos);
        field_pos += 2;

        if (field_pos + fname_len > header_end) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        member.name.assign(reinterpret_cast <const char*>(m_pimpl->data_.data() + field_pos), fname_len);
        field_pos += fname_len;

        // Data follows the header
        size_t data_offset = header_end;

        file_entry entry;
        entry.name = member.name;
        entry.uncompressed_size = member.orig_size;
        entry.compressed_size = member.pack_size;
        entry.datetime = member.datetime;
        entry.folder_index = static_cast <u32>(m_pimpl->members_.size());
        entry.folder_offset = data_offset;

        m_pimpl->files_.push_back(entry);
        m_pimpl->members_.push_back(member);

        pos = header_end + member.pack_size;
        return {};
    }
} // namespace crate
