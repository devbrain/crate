#pragma once

#include <crate/core/types.hh>
#include <crate/core/error.hh>
#include <crate/core/decompressing_stream.hh>
#include <array>
#include <vector>

namespace crate {

// HYP HP Streaming Decompressor
// Adaptive Huffman with dictionary compression
class CRATE_EXPORT hp_decompressor {
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

    explicit hp_decompressor(size_t original_size, u8 version = 0x20);

    void reset();
    void set_original_size(size_t size) { original_size_ = size; }
    void set_version(u8 version) { version_ = version; }

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished
    );

private:
    enum class state {
        BUFFERING_INPUT,      // Collecting input data
        READ_BLOCK_HEADER,    // Read 13-bit teststrings_index
        READ_SMART,           // Decode Huffman data into string buffer
        DECODE_DATA,          // Convert string buffer to output
        BLOCK_COMPLETE,       // Block done, check if more
        DONE
    };

    enum class read_smart_state {
        INIT,
        DECODE_LOOP,
        LSEQUENCE_LOOP,
        DONE
    };

    void init_state();

    // Bit reading
    bool try_read_bit(bool& out);
    bool try_read_bits(u8 count, u16& out);

    // String buffer operations
    [[nodiscard]] u16 read_str(u16 offset) const;
    void write_str(u16 offset, u16 value);

    // Huffman tree operations
    [[nodiscard]] u16 get_sohn(u16 offset) const;
    void set_sohn(u16 offset, u16 value);
    [[nodiscard]] u16 get_vater(u16 offset) const;
    void set_vater(u16 offset, u16 value);
    [[nodiscard]] u16 get_the_freq(u16 offset) const;
    void set_the_freq(u16 offset, u16 value);
    [[nodiscard]] u16 get_freq(u16 offset) const;
    void set_freq(u16 offset, u16 value);
    [[nodiscard]] u16 get_nvalue(u16 offset) const;
    void set_nvalue(u16 offset, u16 value);
    void set_nindex(u16 offset, u16 value);
    [[nodiscard]] u16 get_nfreq(u16 freq_even) const;
    void set_nfreq(u16 freq_even, u16 value);
    [[nodiscard]] u16 get_nfreqmax(u16 freq_even) const;
    void set_nfreqmax(u16 freq_even, u16 value);

    // Huffman operations
    void set_vars();
    void init_huff_tables();
    void ninsert();
    void inc_frequency_ientry(u16 di);
    void inc_posfreq(u16 si);
    void inc_frequency_entry_swap(u16 di, u16 si, u16 bx);
    void inc_frequency(u16 di);
    bool try_decode_huff_entry(u16& out);
    bool try_tab_decode(u16 freq, u16& out);

    // Block operations
    void markiere(u16 di, size_t& counter);
    void clear_when_full();

    // State machine phases
    bool process_read_smart();
    bool process_decode_data(byte*& out_ptr, byte* out_end);

    // Input buffer (for collecting compressed data)
    byte_vector input_buffer_;
    size_t input_pos_ = 0;

    // Bit reading state
    u8 bit_cnt_ = 8;
    u8 puffer_byte_ = 0;

    // Configuration
    size_t original_size_ = 0;
    u8 version_ = 0x20;

    // Output tracking
    size_t bytes_written_ = 0;

    // Huffman tables
    std::vector<u16> vater_;
    std::vector<u16> sohn_;
    std::vector<u16> the_freq_;
    std::vector<u16> nindex_;
    std::vector<u16> nvalue_;
    std::vector<u16> frequencys_;
    std::array<u16, NFREQ_TABLE_LEN> nfreq_{};
    std::vector<u16> nfreqmax_;
    std::vector<u16> str_ind_buf_;
    std::vector<u16> new_index_;

    // Block state
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

    // State machine
    state state_ = state::BUFFERING_INPUT;
    read_smart_state rs_state_ = read_smart_state::INIT;

    // read_smart loop state
    u16 rs_di_ = 0;
    u16 rs_symbol_ = 0;
    u16 rs_pos_ = 0;

    // decode_data state
    u16 dd_si_ = 0;
    u16 dd_ax_ = 0;
    std::vector<u16> dd_stack_;
    bool dd_need_next_entry_ = true;

    // tab_decode state
    u16 td_base_ = 0;
    u16 td_ax_ = 0;
    u16 td_si_ = 0;
    u16 td_dx_ = 0;
    u16 td_cx_ = 0;
    bool td_in_progress_ = false;

    // Huffman decode state
    u16 hd_si_ = 0;
    bool hd_in_progress_ = false;
};

} // namespace crate
