#include <crate/formats/hyp.hh>
#include <crate/formats/hyp_internal.hh>
#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

namespace crate {
    namespace hyp {
        // =============================================================================
        // HP Decompressor - Adaptive Huffman with Dictionary Compression
        // =============================================================================

        class CRATE_EXPORT hp_decoder {
            public:
                static constexpr size_t STR_IND_BUF_LEN = 8191;
                static constexpr size_t TAB_SIZE = STR_IND_BUF_LEN + 200;
                static constexpr size_t HUFF_SIZE = 3200;
                static constexpr size_t MAX_REC_FREQUENCY = 4096;
                static constexpr size_t MAX_FREQ = 2;
                static constexpr size_t MAXMCOUNT = 1350;
                static constexpr size_t NFREQ_TABLE_LEN = 4;
                static constexpr i16 DEFAULT_MINDIFF = -4;
                static constexpr i16 DEFAULT_MAXDIFF = 8;
                static constexpr i16 DIFF_OFFSET = 1;
                static constexpr u16 LSEQUENCE_KEY = 0;
                static constexpr u16 BASE_LASTPOSITION = 2 * 256;
                static constexpr u16 MARKED = 1;
                static constexpr u16 NULL_VAL = 0xFFFF;

                explicit hp_decoder(byte_span data, u8 version)
                    : data_(data), version_(version) {
                    if (!data_.empty()) {
                        puffer_byte_ = data_[0];
                    }
                }

                result_t <byte_vector> decompress(size_t original_size) {
                    byte_vector output;
                    output.reserve(original_size);

                    while (output.size() < original_size) {
                        clear_when_full();
                        auto result = read_smart();
                        if (!result) return std::unexpected(result.error());

                        auto decode_result = decode_data(output, original_size);
                        if (!decode_result) return std::unexpected(decode_result.error());

                        u16 block_end = teststrings_index_ << 1;
                        if (block_end >= 2 * 255) {
                            if (low_tsi_ > block_end) {
                                low_tsi_ = 2 * 255;
                            } else {
                                low_tsi_ = block_end;
                            }
                        }

                        if (teststrings_index_ == 255 || output.size() >= original_size) {
                            break;
                        }

                        if (is_exhausted()) {
                            break;
                        }
                    }

                    output.resize(std::min(output.size(), original_size));
                    return output;
                }

            private:
                // Bit reading (LSB first)
                bool read_bit() {
                    bool bit = (puffer_byte_ & 1) != 0;
                    puffer_byte_ >>= 1;
                    bit_cnt_--;
                    if (bit_cnt_ == 0) {
                        byte_pos_++;
                        puffer_byte_ = (byte_pos_ < data_.size()) ? data_[byte_pos_] : 0;
                        bit_cnt_ = 8;
                    }
                    return bit;
                }

                u16 read_bits(u8 count) {
                    u16 value = 0;
                    for (u8 i = 0; i < count; i++) {
                        if (read_bit()) {
                            value |= (1u << i);
                        }
                    }
                    return value;
                }

                [[nodiscard]] bool is_exhausted() const {
                    return byte_pos_ >= data_.size();
                }

                // String buffer operations
                [[nodiscard]] u16 read_str(u16 offset) const {
                    size_t idx = offset >> 1;
                    return (idx < str_ind_buf_.size()) ? str_ind_buf_[idx] : 0;
                }

                void write_str(u16 offset, u16 value) {
                    size_t idx = offset >> 1;
                    if (idx < str_ind_buf_.size()) {
                        str_ind_buf_[idx] = value;
                    }
                }

                // Huffman tree operations
                [[nodiscard]] u16 get_sohn(u16 offset) const {
                    size_t idx = offset >> 1;
                    return (idx < sohn_.size()) ? sohn_[idx] : 0;
                }

                void set_sohn(u16 offset, u16 value) {
                    size_t idx = offset >> 1;
                    if (idx < sohn_.size()) sohn_[idx] = value;
                }

                [[nodiscard]] u16 get_vater(u16 offset) const {
                    size_t idx = offset >> 1;
                    return (idx < vater_.size()) ? vater_[idx] : 0;
                }

                void set_vater(u16 offset, u16 value) {
                    size_t idx = offset >> 1;
                    if (idx < vater_.size()) vater_[idx] = value;
                }

                [[nodiscard]] u16 get_the_freq(u16 offset) const {
                    size_t idx = offset >> 1;
                    return (idx < the_freq_.size()) ? the_freq_[idx] : 0;
                }

                void set_the_freq(u16 offset, u16 value) {
                    size_t idx = offset >> 1;
                    if (idx < the_freq_.size()) the_freq_[idx] = value;
                }

                [[nodiscard]] u16 get_freq(u16 offset) const {
                    size_t idx = offset >> 1;
                    return (idx < frequencys_.size()) ? frequencys_[idx] : 0;
                }

                void set_freq(u16 offset, u16 value) {
                    size_t idx = offset >> 1;
                    if (idx < frequencys_.size()) frequencys_[idx] = value;
                }

                [[nodiscard]] u16 get_nvalue(u16 offset) const {
                    size_t idx = offset >> 1;
                    return (idx < nvalue_.size()) ? nvalue_[idx] : 0;
                }

                void set_nvalue(u16 offset, u16 value) {
                    size_t idx = offset >> 1;
                    if (idx < nvalue_.size()) nvalue_[idx] = value;
                }

                void set_nindex(u16 offset, u16 value) {
                    size_t idx = offset >> 1;
                    if (idx < nindex_.size()) nindex_[idx] = value;
                }

                [[nodiscard]] u16 get_nfreq(u16 freq_even) const {
                    size_t slot = freq_even / 2;
                    return (slot < NFREQ_TABLE_LEN) ? nfreq_[slot] : 0;
                }

                void set_nfreq(u16 freq_even, u16 value) {
                    size_t slot = freq_even / 2;
                    if (slot < NFREQ_TABLE_LEN) nfreq_[slot] = value;
                }

                [[nodiscard]] u16 get_nfreqmax(u16 freq_even) const {
                    if (freq_even < 2) return 0;
                    size_t slot = (freq_even / 2) - 1;
                    return (slot < nfreqmax_.size()) ? nfreqmax_[slot] : 0;
                }

                void set_nfreqmax(u16 freq_even, u16 value) {
                    if (freq_even < 2) return;
                    size_t slot = (freq_even / 2) - 1;
                    if (slot < nfreqmax_.size()) nfreqmax_[slot] = value;
                }

                void markiere(u16 di, size_t& counter) {
                    size_t idx = di >> 1;
                    if (idx >= new_index_.size() || new_index_[idx] != 0) return;

                    u16 si = read_str(di);
                    if (si >= 512) {
                        if (si >= 2) markiere(si - 2, counter);
                        if (si != di) markiere(si, counter);
                    }
                    new_index_[idx] = MARKED;
                    counter++;
                }

                void clear_when_full() {
                    if (teststrings_index_ != STR_IND_BUF_LEN) return;

                    std::fill(new_index_.begin(), new_index_.end(), 0);
                    size_t count = 0;
                    u16 di = static_cast <u16>(2 * STR_IND_BUF_LEN);
                    while (di > 0) {
                        di -= 2;
                        markiere(di, count);
                        if (count >= MAXMCOUNT || di == 0) break;
                    }

                    low_tsi_ = static_cast <u16>(std::min((count + 255) * 2, size_t(0xFFFF)));

                    u16 bx = 2 * 255;
                    u16 di_iter = 2 * 254;
                    while (bx < low_tsi_) {
                        di_iter += 2;
                        size_t idx = di_iter >> 1;
                        if (idx >= new_index_.size()) break;
                        if (new_index_[idx] == 0) continue;

                        new_index_[idx] = bx;
                        u16 si = read_str(di_iter);
                        if (si >= 512) {
                            size_t map_idx = si >> 1;
                            si = (map_idx < new_index_.size()) ? new_index_[map_idx] : 0;
                        }

                        write_str(bx, si);
                        bx += 2;
                    }
                }

                void set_vars() {
                    u8 major = version_ >> 4;

                    if (major == 3) {
                        mindiff_ = -8;
                        maxdiff_ = 8;
                        maxlocal_ = 0x0096;
                    } else {
                        mindiff_ = -4;
                        maxdiff_ = 8;

                        i32 bx_i32 = static_cast <i16>(teststrings_index_ - 0x00ff);
                        i32 prod = 0x009c * bx_i32;
                        i32 quot = prod / 0x1f00;
                        u16 ax_u16 = (static_cast <u16>(quot) & 0xfffe) + 4;
                        maxlocal_ = ax_u16;
                    }

                    maxlocal255_ = maxlocal_ + 0x01fe;
                    local_offset_ = static_cast <u16>(4 + maxdiff_ - mindiff_);
                    pos_offset_ = static_cast <u16>(local_offset_ + maxlocal_ + 2);
                    char_offset_ = pos_offset_ + 2 * (static_cast <u16>(MAX_FREQ) + 1);
                }

                void init_huff_tables() {
                    std::fill(nfreqmax_.begin(), nfreqmax_.end(), 0);

                    u16 di = char_offset_;
                    u16 si = 0;
                    for (size_t i = 0; i < STR_IND_BUF_LEN; i++) {
                        size_t index = si >> 1;
                        if (index >= nindex_.size()) break;
                        nindex_[index] = di;

                        size_t dest = di >> 1;
                        if (dest < nvalue_.size()) {
                            nvalue_[dest] = si;
                            frequencys_[dest] = 0;
                        }

                        di += 2;
                        si += 2;
                    }

                    for (auto& slot : nfreq_) {
                        slot = char_offset_;
                    }

                    nfreq_[3] = 2;
                    sohn_[0] = 1;
                    the_freq_[0] = 2;
                    vater_[0] = 0;
                    huff_max_ = 2;
                    huff_max_index_ = 2;
                    frequencys_[0] = 0;

                    u16 count = (char_offset_ / 2);
                    if (count > 0) count--;
                    while (count > 0) {
                        ninsert();
                        count--;
                    }
                }

                void ninsert() {
                    u16 di = huff_max_;
                    u16 parent = di - 2;

                    set_vater(di, parent);
                    set_vater(di + 2, parent);
                    set_the_freq(di + 2, 2);

                    u16 leaf_marker = nfreq_[3] + 1;
                    set_sohn(di + 2, leaf_marker);

                    u16 old_child = get_sohn(parent);
                    set_sohn(parent, di);
                    set_sohn(di, old_child);

                    if (old_child != 0) {
                        set_freq(old_child - 1, di);
                    }

                    u16 parent_freq = get_the_freq(parent);
                    set_the_freq(di, parent_freq);

                    u16 leaf_slot = di + 2;
                    set_freq(nfreq_[3], leaf_slot);

                    nfreq_[3] += 2;
                    huff_max_ = leaf_slot + 2;

                    inc_frequency_ientry(parent);
                }

                void inc_frequency_ientry(u16 di) {
                    u16 limit = 2 * MAX_REC_FREQUENCY;

                    while (true) {
                        u16 freq = get_the_freq(di);
                        if (freq >= limit) return;

                        u16 si = get_nfreqmax(freq);
                        set_nfreqmax(freq, si + 2);

                        if (di != si) {
                            u16 bx_child = get_sohn(di);
                            set_vater(bx_child, si);
                            set_vater(bx_child + 2, si);
                            inc_frequency_entry_swap(di, si, bx_child);
                        }

                        set_the_freq(si, get_the_freq(si) + 2);
                        di = get_vater(si);
                        if (si == 0) return;
                    }
                }

                void inc_posfreq(u16 si) {
                    u16 bx = get_freq(si);
                    u16 di = get_nfreq(bx + 2);

                    u16 item = get_nvalue(si);
                    set_nindex(item, di);

                    u16 swapped = get_nvalue(di);
                    set_nvalue(di, item);
                    item = swapped;

                    set_nindex(item, si);
                    set_nvalue(si, item);

                    bx = get_freq(di);
                    if (bx == 2 * MAX_FREQ) {
                        ninsert();
                        u16 di_next = huff_max_ - 2;
                        inc_frequency(di_next);
                        return;
                    }

                    bx += 2;
                    set_freq(di, bx);
                    di += 2;
                    set_nfreq(bx, di);
                }

                void inc_frequency_entry_swap(u16 di, u16 si, u16 bx) {
                    u16 old = get_sohn(si);
                    set_sohn(si, bx);
                    bx = old;

                    if (bx & 1) {
                        set_freq(bx - 1, di);
                    } else {
                        set_vater(bx, di);
                        set_vater(bx + 2, di);
                    }

                    set_sohn(di, bx);
                }

                void inc_frequency(u16 di) {
                    u16 limit = 2 * MAX_REC_FREQUENCY;

                    while (true) {
                        u16 freq = get_the_freq(di);
                        if (freq >= limit) return;

                        u16 si = get_nfreqmax(freq);
                        set_nfreqmax(freq, si + 2);

                        if (di != si) {
                            u16 bx_child = get_sohn(di);
                            if (bx_child & 1) {
                                set_freq(bx_child - 1, si);
                            } else {
                                set_vater(bx_child, si);
                                set_vater(bx_child + 2, si);
                            }
                            inc_frequency_entry_swap(di, si, bx_child);
                        }

                        set_the_freq(si, get_the_freq(si) + 2);
                        di = get_vater(si);
                        if (si == 0) return;
                    }
                }

                std::optional <u16> decode_huff_entry() {
                    u16 si = 0;

                    while (true) {
                        u16 child = get_sohn(si);
                        if (child & 1) {
                            u16 leaf = child - 1;
                            u16 freq_ptr = get_freq(leaf);
                            inc_frequency(freq_ptr);
                            return leaf;
                        }

                        bool bit = read_bit();
                        si = bit ? (child + 2) : child;
                    }
                }

                u16 tab_decode(u16 freq) {
                    u16 base = freq - pos_offset_;

                    u16 ax = get_nfreq(base);
                    u16 si = get_nfreq(base + 2);
                    ax -= si;
                    ax >>= 1;
                    u16 dx = ax;

                    u16 cx = 1;
                    ax = 1;

                    while (true) {
                        if (!read_bit()) {
                            ax ^= cx;
                        }
                        cx <<= 1;
                        ax |= cx;
                        if (ax <= dx) continue;
                        ax ^= cx;
                        break;
                    }

                    ax <<= 1;
                    si += ax;
                    u16 value = get_nvalue(si);
                    inc_posfreq(si);
                    return value;
                }

                void_result_t read_smart() {
                    u16 tsi = read_bits(13) & 0x1fff;
                    teststrings_index_ = tsi;
                    set_vars();
                    init_huff_tables();

                    teststrings_index_ <<= 1;
                    u16 di = low_tsi_ - 2;
                    lastposition_ = BASE_LASTPOSITION;
                    nfreq_[0] = char_offset_ + 2 * 255;

                    while (true) {
                        di += 2;
                        if (di >= teststrings_index_) break;

                        if (di > maxlocal255_) {
                            nfreq_[0] = static_cast <u16>(di + char_offset_ - maxlocal_);
                        }

                        auto symbol_opt = decode_huff_entry();
                        if (!symbol_opt) break;
                        u16 symbol = *symbol_opt;

                        if (symbol == 2 * LSEQUENCE_KEY) {
                            u16 pos = lastposition_ + 4;
                            write_str(di, pos);

                            di += 2;
                            while (true) {
                                pos += 4;
                                write_str(di, pos);
                                di += 2;

                                if (read_bit()) continue;

                                di -= 2;
                                break;
                            }

                            lastposition_ = pos;
                        } else if (symbol < local_offset_) {
                            u16 adjust = static_cast <u16>(2 * DIFF_OFFSET - mindiff_);
                            u16 new_pos = static_cast <u16>(lastposition_ + symbol - adjust);
                            write_str(di, new_pos);
                            lastposition_ = new_pos;
                        } else if (symbol < pos_offset_) {
                            u16 offset = symbol - local_offset_;
                            u16 new_pos = di - offset;
                            write_str(di, new_pos);
                            lastposition_ = new_pos;
                        } else if (symbol < char_offset_) {
                            u16 value = tab_decode(symbol);
                            if (value >= 512) {
                                write_str(di, value);
                                lastposition_ = value;
                            } else {
                                write_str(di, value >> 1);
                            }
                        } else {
                            u16 value = get_nvalue(symbol);
                            if (value >= 512) {
                                write_str(di, value);
                                lastposition_ = value;
                            } else {
                                write_str(di, value >> 1);
                            }
                        }
                    }

                    teststrings_index_ >>= 1;
                    return {};
                }

                void_result_t decode_data(byte_vector& output, size_t target_len) const {
                    u16 si = low_tsi_;
                    u16 tsi_bytes = teststrings_index_ << 1;
                    std::vector <u16> stack;
                    stack.reserve(64);

                    while (si < tsi_bytes && output.size() < target_len) {
                        u16 ax = read_str(si);
                        si += 2;

                        size_t steps = 0;
                        while (true) {
                            steps++;
                            if (steps > 65536) {
                                return std::unexpected(error{
                                    error_code::CorruptData, "HYP decode step limit exceeded"
                                });
                            }

                            if ((ax & 0xFF00) == 0) {
                                u8 value = static_cast <u8>(ax & 0x00FF);
                                output.push_back(value);
                                if (output.size() >= target_len) {
                                    return {};
                                }

                                if (!stack.empty()) {
                                    ax = stack.back();
                                    stack.pop_back();
                                    continue;
                                }
                                break;
                            } else {
                                u16 bx = ax;
                                u16 tail = read_str(bx);
                                if (tail == bx) {
                                    tail = read_str(bx - 2);
                                }
                                stack.push_back(tail);
                                ax = read_str(bx - 2);
                            }
                        }
                    }

                    return {};
                }

                // Data source
                byte_span data_;
                size_t byte_pos_ = 0;
                u8 bit_cnt_ = 8;
                u8 puffer_byte_ = 0;

                // Version
                u8 version_ = 0;

                // Huffman tables
                std::vector <u16> vater_ = std::vector <u16>(2 * HUFF_SIZE + 2, 0);
                std::vector <u16> sohn_ = std::vector <u16>(2 * HUFF_SIZE + 2, 0);
                std::vector <u16> the_freq_ = std::vector <u16>(2 * HUFF_SIZE + 2, 0);
                std::vector <u16> nindex_ = std::vector <u16>(STR_IND_BUF_LEN + 2, 0);
                std::vector <u16> nvalue_ = std::vector <u16>(TAB_SIZE + 2, 0);
                std::vector <u16> frequencys_ = std::vector <u16>(TAB_SIZE + 2, 0);
                std::array <u16, NFREQ_TABLE_LEN> nfreq_{};
                std::vector <u16> nfreqmax_ = std::vector <u16>(MAX_REC_FREQUENCY + 1, 0);
                std::vector <u16> str_ind_buf_ = std::vector <u16>(STR_IND_BUF_LEN + 1, 0);
                std::vector <u16> new_index_ = std::vector <u16>(STR_IND_BUF_LEN + 1, 0);

                // State
                u16 teststrings_index_ = 255;
                u16 low_tsi_ = 2 * 255;
                u16 lastposition_ = BASE_LASTPOSITION;
                u16 huff_max_ = 2;
                u16 huff_max_index_ = 2;
                u16 maxlocal_ = 0;
                u16 maxlocal255_ = 0;
                u16 local_offset_ = 0;
                u16 pos_offset_ = 0;
                u16 char_offset_ = 0;
                i16 mindiff_ = DEFAULT_MINDIFF;
                i16 maxdiff_ = DEFAULT_MAXDIFF;
        };

        inline result_t <byte_vector> decompress_hp(byte_span data, size_t original_size, u8 version) {
            hp_decoder decoder(data, version);
            return decoder.decompress(original_size);
        }

        constexpr u8 HYP_ID = 0x1A;
        constexpr u16 STORED_MAGIC = 0x5453; // "ST" in little-endian
        constexpr u16 COMPRESSED_MAGIC = 0x5048; // "HP" in little-endian
        constexpr size_t HEADER_SIZE = 21;

        enum method : u16 {
            STORED = STORED_MAGIC,
            COMPRESSED = COMPRESSED_MAGIC
        };

        struct CRATE_EXPORT file_header {
            u16 method = 0;
            u8 version = 0;
            u32 compressed_size = 0;
            u32 original_size = 0;
            dos_date_time datetime{};
            u32 checksum = 0;
            u8 attribute = 0;
            std::string name;
        };
    }

    struct hyp_archive::impl {
        byte_vector data_;
        std::vector <hyp::file_header> members_;
        std::vector <file_entry> files_;
    };

    hyp_archive::hyp_archive()
        : m_pimpl(std::make_unique <impl>()) {
    }

    hyp_archive::~hyp_archive() = default;

    result_t <std::unique_ptr <hyp_archive>> hyp_archive::open(byte_span data) {
        auto archive = std::unique_ptr <hyp_archive>(new hyp_archive());
        archive->m_pimpl->data_.assign(data.begin(), data.end());

        auto result = archive->parse();
        if (!result) return std::unexpected(result.error());

        return archive;
    }

    result_t <std::unique_ptr <hyp_archive>> hyp_archive::open(const std::filesystem::path& path) {
        auto file = file_input_stream::open(path);
        if (!file) return std::unexpected(file.error());

        auto size = file->size();
        if (!size) return std::unexpected(size.error());

        byte_vector data(*size);
        auto read = file->read(data);
        if (!read) return std::unexpected(read.error());

        return open(data);
    }

    const std::vector <file_entry>& hyp_archive::files() const { return m_pimpl->files_; }

    result_t <byte_vector> hyp_archive::extract(const file_entry& entry) {
        if (entry.folder_index >= m_pimpl->members_.size()) {
            return std::unexpected(error{error_code::FileNotInArchive});
        }

        const auto& member = m_pimpl->members_[entry.folder_index];
        if (entry.folder_offset + member.compressed_size > m_pimpl->data_.size()) {
            return std::unexpected(error{error_code::TruncatedArchive});
        }

        byte_span compressed(m_pimpl->data_.data() + entry.folder_offset, member.compressed_size);

        // Verify checksum on compressed data
        u32 calc_checksum = hyp::hyp_checksum(compressed);
        if (calc_checksum != member.checksum) {
            return std::unexpected(error{error_code::InvalidChecksum, "HYP checksum mismatch"});
        }

        byte_vector output;

        switch (member.method) {
            case hyp::STORED:
                output.assign(compressed.begin(), compressed.end());
                break;

            case hyp::COMPRESSED: {
                auto result = hyp::decompress_hp(compressed, member.original_size, member.version);
                if (!result) return std::unexpected(result.error());
                output = std::move(*result);
                break;
            }

            default:
                return std::unexpected(error{
                    error_code::UnsupportedCompression,
                    "Unknown HYP compression method"
                });
        }

        return output;
    }

    void_result_t hyp_archive::parse() {
        size_t pos = 0;

        while (pos + 1 + hyp::HEADER_SIZE < m_pimpl->data_.size()) {
            // Check for HYP ID byte
            if (m_pimpl->data_[pos] != hyp::HYP_ID) {
                break;
            }
            pos++;

            if (pos + hyp::HEADER_SIZE > m_pimpl->data_.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }

            hyp::file_header member;
            member.method = read_u16_le(m_pimpl->data_.data() + pos);
            pos += 2;
            member.version = m_pimpl->data_[pos++];
            member.compressed_size = read_u32_le(m_pimpl->data_.data() + pos);
            pos += 4;
            member.original_size = read_u32_le(m_pimpl->data_.data() + pos);
            pos += 4;

            u32 datetime = read_u32_le(m_pimpl->data_.data() + pos);
            member.datetime.date = static_cast <u16>(datetime & 0xFFFF);
            member.datetime.time = static_cast <u16>((datetime >> 16) & 0xFFFF);
            pos += 4;

            member.checksum = read_u32_le(m_pimpl->data_.data() + pos);
            pos += 4;
            member.attribute = m_pimpl->data_[pos++];

            // Read name length and name
            u8 name_length = m_pimpl->data_[pos++];
            if (pos + name_length > m_pimpl->data_.size()) {
                return std::unexpected(error{error_code::TruncatedArchive});
            }

            member.name.assign(reinterpret_cast <const char*>(m_pimpl->data_.data() + pos), name_length);
            pos += name_length;

            // Store data offset
            size_t data_offset = pos;

            file_entry entry;
            entry.name = member.name;
            entry.uncompressed_size = member.original_size;
            entry.compressed_size = member.compressed_size;
            entry.datetime = member.datetime;
            entry.folder_index = static_cast <u32>(m_pimpl->members_.size());
            entry.folder_offset = data_offset;

            m_pimpl->files_.push_back(entry);
            m_pimpl->members_.push_back(member);

            pos += member.compressed_size;
        }

        // If no files found, this is not a valid HYP archive
        if (m_pimpl->files_.empty()) {
            return std::unexpected(error{error_code::InvalidSignature, "Not a HYP archive"});
        }

        return {};
    }
} // namespace crate
