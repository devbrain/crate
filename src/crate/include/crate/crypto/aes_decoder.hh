#pragma once

#include <crate/core/types.hh>
#include <array>
#include <cstring>

namespace crate::crypto {
    // AES (Rijndael) implementation supporting 128, 192, and 256-bit keys
    // Implements CBC mode decryption for RAR archive support
    class CRATE_EXPORT aes_decoder {
        public:
            static constexpr size_t BLOCK_SIZE = 16;

            // Initialize for decryption with key and IV
            // key_bits must be 128, 192, or 256
            void init_decrypt(const u8* key, unsigned key_bits, const u8* iv) {
                key_bits_ = key_bits;
                std::memcpy(iv_.data(), iv, BLOCK_SIZE);
                expand_key_decrypt(key, key_bits);
            }

            // Decrypt data in-place (CBC mode)
            // Length must be multiple of 16
            void decrypt_cbc(u8* data, size_t length) {
                std::array <u8, BLOCK_SIZE> prev_cipher{};
                std::memcpy(prev_cipher.data(), iv_.data(), BLOCK_SIZE);

                for (size_t offset = 0; offset < length; offset += BLOCK_SIZE) {
                    std::array <u8, BLOCK_SIZE> cipher_block{};
                    std::memcpy(cipher_block.data(), data + offset, BLOCK_SIZE);

                    decrypt_block(data + offset);

                    // XOR with previous ciphertext (or IV for first block)
                    for (size_t i = 0; i < BLOCK_SIZE; i++) {
                        data[offset + i] ^= prev_cipher[i];
                    }

                    prev_cipher = cipher_block;
                }

                // Update IV for chained calls
                std::memcpy(iv_.data(), prev_cipher.data(), BLOCK_SIZE);
            }

            // Decrypt a single block in-place
            void decrypt_block(u8* block) {
                std::array <u8, BLOCK_SIZE> state{};
                std::memcpy(state.data(), block, BLOCK_SIZE);

                unsigned rounds = (key_bits_ == 128) ? 10 : ((key_bits_ == 192) ? 12 : 14);

                // Initial round key addition
                add_round_key(state, rounds);

                // Main rounds (in reverse)
                for (unsigned round = rounds - 1; round > 0; round--) {
                    inv_shift_rows(state);
                    inv_sub_bytes(state);
                    add_round_key(state, round);
                    inv_mix_columns(state);
                }

                // Final round
                inv_shift_rows(state);
                inv_sub_bytes(state);
                add_round_key(state, 0);

                std::memcpy(block, state.data(), BLOCK_SIZE);
            }

        private:
            // AES S-box
            static constexpr u8 SBOX[256] = {
                0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
                0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
                0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
                0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
                0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
                0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
                0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
                0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
                0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
                0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
                0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
                0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
                0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
                0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
                0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
                0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
            };

            // Inverse S-box
            static constexpr u8 INV_SBOX[256] = {
                0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
                0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
                0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
                0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
                0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
                0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
                0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
                0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
                0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
                0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
                0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
                0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
                0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
                0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
                0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
                0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d
            };

            // Round constants
            static constexpr u8 RCON[11] = {
                0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
            };

            void expand_key_decrypt(const u8* key, unsigned key_bits) {
                unsigned nk = key_bits / 32; // Key length in 32-bit words
                unsigned nr = nk + 6; // Number of rounds
                unsigned nb = 4; // Block size in words (always 4 for AES)

                // Copy original key
                for (unsigned i = 0; i < nk; i++) {
                    round_key_[i * 4 + 0] = key[i * 4 + 0];
                    round_key_[i * 4 + 1] = key[i * 4 + 1];
                    round_key_[i * 4 + 2] = key[i * 4 + 2];
                    round_key_[i * 4 + 3] = key[i * 4 + 3];
                }

                // Expand key
                for (unsigned i = nk; i < nb * (nr + 1); i++) {
                    u8 temp[4];
                    temp[0] = round_key_[(i - 1) * 4 + 0];
                    temp[1] = round_key_[(i - 1) * 4 + 1];
                    temp[2] = round_key_[(i - 1) * 4 + 2];
                    temp[3] = round_key_[(i - 1) * 4 + 3];

                    if (i % nk == 0) {
                        // RotWord + SubWord + Rcon
                        u8 t = temp[0];
                        temp[0] = SBOX[temp[1]] ^ RCON[i / nk];
                        temp[1] = SBOX[temp[2]];
                        temp[2] = SBOX[temp[3]];
                        temp[3] = SBOX[t];
                    } else if (nk > 6 && i % nk == 4) {
                        // SubWord for 256-bit keys
                        temp[0] = SBOX[temp[0]];
                        temp[1] = SBOX[temp[1]];
                        temp[2] = SBOX[temp[2]];
                        temp[3] = SBOX[temp[3]];
                    }

                    round_key_[i * 4 + 0] = round_key_[(i - nk) * 4 + 0] ^ temp[0];
                    round_key_[i * 4 + 1] = round_key_[(i - nk) * 4 + 1] ^ temp[1];
                    round_key_[i * 4 + 2] = round_key_[(i - nk) * 4 + 2] ^ temp[2];
                    round_key_[i * 4 + 3] = round_key_[(i - nk) * 4 + 3] ^ temp[3];
                }
            }

            void add_round_key(std::array <u8, BLOCK_SIZE>& state, unsigned round) const {
                for (unsigned i = 0; i < BLOCK_SIZE; i++) {
                    state[i] ^= round_key_[round * BLOCK_SIZE + i];
                }
            }

            static void inv_sub_bytes(std::array <u8, BLOCK_SIZE>& state) {
                for (unsigned i = 0; i < BLOCK_SIZE; i++) {
                    state[i] = INV_SBOX[state[i]];
                }
            }

            static void inv_shift_rows(std::array <u8, BLOCK_SIZE>& state) {
                u8 temp;

                // Row 1: shift right by 1
                temp = state[13];
                state[13] = state[9];
                state[9] = state[5];
                state[5] = state[1];
                state[1] = temp;

                // Row 2: shift right by 2
                temp = state[2];
                state[2] = state[10];
                state[10] = temp;
                temp = state[6];
                state[6] = state[14];
                state[14] = temp;

                // Row 3: shift right by 3 (= left by 1)
                temp = state[3];
                state[3] = state[7];
                state[7] = state[11];
                state[11] = state[15];
                state[15] = temp;
            }

            static u8 gf_mul(u8 a, u8 b) {
                u8 result = 0;
                u8 hi_bit;
                for (int i = 0; i < 8; i++) {
                    if (b & 1) {
                        result ^= a;
                    }
                    hi_bit = a & 0x80;
                    a <<= 1;
                    if (hi_bit) {
                        a ^= 0x1b; // AES irreducible polynomial
                    }
                    b >>= 1;
                }
                return result;
            }

            static void inv_mix_columns(std::array <u8, BLOCK_SIZE>& state) {
                for (unsigned col = 0; col < 4; col++) {
                    u8 a0 = state[col * 4 + 0];
                    u8 a1 = state[col * 4 + 1];
                    u8 a2 = state[col * 4 + 2];
                    u8 a3 = state[col * 4 + 3];

                    state[col * 4 + 0] = gf_mul(a0, 0x0e) ^ gf_mul(a1, 0x0b) ^ gf_mul(a2, 0x0d) ^ gf_mul(a3, 0x09);
                    state[col * 4 + 1] = gf_mul(a0, 0x09) ^ gf_mul(a1, 0x0e) ^ gf_mul(a2, 0x0b) ^ gf_mul(a3, 0x0d);
                    state[col * 4 + 2] = gf_mul(a0, 0x0d) ^ gf_mul(a1, 0x09) ^ gf_mul(a2, 0x0e) ^ gf_mul(a3, 0x0b);
                    state[col * 4 + 3] = gf_mul(a0, 0x0b) ^ gf_mul(a1, 0x0d) ^ gf_mul(a2, 0x09) ^ gf_mul(a3, 0x0e);
                }
            }

            unsigned key_bits_ = 128;
            std::array <u8, BLOCK_SIZE> iv_{};
            std::array <u8, 240> round_key_{}; // Max size for AES-256 (15 rounds * 16 bytes)
    };
} // namespace crate::crypto
