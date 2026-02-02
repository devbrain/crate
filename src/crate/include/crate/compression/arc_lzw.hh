#pragma once

#include <crate/core/decompressor.hh>
#include <array>
#include <vector>

namespace crate {

// ARC LZW decompressor - streaming implementation
// Supports 12-bit (crunched) and 13-bit (squashed) modes
// Based on Unix compress algorithm with variable code size
class CRATE_EXPORT arc_lzw_decompressor : public bounded_decompressor {
public:
    explicit arc_lzw_decompressor(bool squashed = false)
        : squashed_(squashed) {
        init_state();
    }

    result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) override;

    void reset() override {
        init_state();
        clear_expected_output_size();
    }

    // Configuration
    void set_squashed(bool squashed) { squashed_ = squashed; }
    [[nodiscard]] bool is_squashed() const { return squashed_; }

private:
    static constexpr int INIT_BITS = 9;
    static constexpr int CLEAR = 256;
    static constexpr int FIRST = 257;
    static constexpr int MAX_CODE = 8191;

    enum class state : u8 {
        SKIP_HEADER,     // For crunched, skip first byte
        READ_FIRST_CODE,
        OUTPUT_FIRST_CHAR,
        READ_CODE,
        HANDLE_CLEAR,
        OUTPUT_AFTER_CLEAR,
        BUILD_STRING,
        OUTPUT_STRING,
        ADD_TO_DICT,
        DONE
    };

    void init_state();
    void init_dictionary();

    // Streaming bit reader
    bool try_read_code(const byte*& ptr, const byte* end, int& code);

    // Configuration
    bool squashed_ = false;  // 13-bit mode (no RLE post-processing)

    // State machine
    state state_ = state::SKIP_HEADER;

    // Bit buffer for streaming
    u32 bit_buffer_ = 0;
    unsigned bits_left_ = 0;

    // LZW parameters
    int max_bits_ = 12;  // 12 for crunched, 13 for squashed
    int n_bits_ = INIT_BITS;
    int max_code_ = (1 << INIT_BITS) - 1;
    int free_ent_ = FIRST;

    // Dictionary
    std::array<u16, MAX_CODE + 1> prefix_{};
    std::array<u8, MAX_CODE + 1> suffix_{};

    // Current operation state
    int code_ = 0;
    int oldcode_ = 0;
    int incode_ = 0;
    u8 finchar_ = 0;

    // Output stack for reversed string
    std::vector<u8> stack_;
    size_t stack_pos_ = 0;
};

}  // namespace crate
