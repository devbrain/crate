#pragma once

#include <crate/core/decompressor.hh>
#include <crate/core/bitstream.hh>
#include <crate/core/huffman.hh>
#include <array>

namespace crate {
    // MSZIP constants
    constexpr size_t MSZIP_BLOCK_SIZE = 32768;

    // Deflate extra bits tables
    namespace deflate {
        constexpr std::array <u8, 29> length_extra_bits = {
            0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
            3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
        };

        constexpr std::array <u16, 29> length_base = {
            3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
            35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
        };

        constexpr std::array <u8, 30> distance_extra_bits = {
            0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
            7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
        };

        constexpr std::array <u16, 30> distance_base = {
            1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
            257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
            8193, 12289, 16385, 24577
        };

        // Code length alphabet order
        constexpr std::array <u8, 19> code_length_order = {
            16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
        };
    }

    class mszip_decompressor : public decompressor {
        public:
            mszip_decompressor();

            result_t<stream_result> decompress_some(
                byte_span input,
                mutable_byte_span output,
                bool input_finished = false
            ) override;

            void reset() override;

        private:
            void init_state();
            void update_history(u8 value);

            u8 get_history(size_t distance) const;

            void build_fixed_tables();

            void_result_t read_dynamic_tables(lsb_bitstream& bs);

            void_result_t decompress_block(lsb_bitstream& bs, mutable_byte_span output, size_t& out_pos);

            literal_decoder literal_decoder_;
            distance_decoder distance_decoder_;
            std::array <u8, MSZIP_BLOCK_SIZE> history_{};
            size_t history_pos_ = 0;
    };
} // namespace crate
