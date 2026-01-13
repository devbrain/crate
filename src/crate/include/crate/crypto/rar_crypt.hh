#pragma once

#include <crate/crypto/aes_decoder.hh>
#include <crate/crypto/sha.hh>
#include <crate/core/types.hh>
#include <array>
#include <cstring>
#include <string>

namespace crate::crypto {
    // RAR4 (v3.x) encryption constants
    namespace rar4 {
        constexpr size_t SALT_SIZE = 8;
        constexpr size_t KEY_SIZE = 16; // AES-128
        constexpr size_t IV_SIZE = 16;
        constexpr unsigned ROUNDS = 0x40000; // 262,144 iterations
    }

    // RAR5 encryption constants
    namespace rar5 {
        constexpr size_t SALT_SIZE = 16;
        constexpr size_t KEY_SIZE = 32; // AES-256
        constexpr size_t IV_SIZE = 16;
        constexpr size_t PWD_CHECK_SIZE = 8;
        constexpr size_t PWD_CHECK_SUM_SIZE = 4;
        constexpr unsigned DEFAULT_KDF_COUNT = 15; // 2^15 = 32768 iterations
    }

    // RAR4 key derivation (SHA-1 based, 262144 rounds)
    // Password is UTF-16LE encoded
    class CRATE_EXPORT rar4_key_derivation {
        public:
            // Derive key and IV from password and salt
            // password: UTF-16LE encoded password bytes
            // password_len: length in bytes (not characters)
            static void derive(const u8* password, size_t password_len,
                               const u8* salt, size_t salt_len,
                               u8* key, u8* iv) {
                // The RAR3 algorithm collects IV bytes at specific intervals
                // during the 262144 iterations
                std::array <u8, sha1_hasher::DIGEST_SIZE> digest{};
                std::array <u8, rar4::IV_SIZE> iv_collect{};

                sha1_hasher sha;

                for (unsigned round = 0; round < rar4::ROUNDS; round++) {
                    sha.reset();

                    // Hash: password + salt + round_counter(3 bytes LE)
                    sha.update(password, password_len);
                    sha.update(salt, salt_len);

                    u8 round_bytes[3] = {
                        static_cast <u8>(round),
                        static_cast <u8>(round >> 8),
                        static_cast <u8>(round >> 16)
                    };
                    sha.update(round_bytes, 3);

                    sha.finalize(digest.data());

                    // Every (ROUNDS/16) iterations, save a byte from digest[4]
                    // (the 5th word of SHA-1 output) for the IV
                    if ((round & 0x3FFF) == 0) {
                        unsigned iv_idx = round >> 14;
                        if (iv_idx < rar4::IV_SIZE) {
                            iv_collect[iv_idx] = digest[4 * 4]; // First byte of 5th word
                        }
                    }
                }

                // Final hash to get the key
                sha.reset();
                sha.update(password, password_len);
                sha.update(salt, salt_len);

                u8 final_round[3] = {
                    static_cast <u8>(rar4::ROUNDS),
                    static_cast <u8>(rar4::ROUNDS >> 8),
                    static_cast <u8>(rar4::ROUNDS >> 16)
                };
                sha.update(final_round, 3);
                sha.finalize(digest.data());

                // Extract key from digest (first 16 bytes)
                std::memcpy(key, digest.data(), rar4::KEY_SIZE);

                // Copy collected IV bytes
                std::memcpy(iv, iv_collect.data(), rar4::IV_SIZE);
            }

            // Convenience function with string password (converts to UTF-16LE)
            static void derive(const std::string& password,
                               const u8* salt, size_t salt_len,
                               u8* key, u8* iv) {
                // Convert password to UTF-16LE
                std::vector <u8> utf16;
                utf16.reserve(password.size() * 2);

                for (char c : password) {
                    utf16.push_back(static_cast <u8>(c));
                    utf16.push_back(0); // High byte is 0 for ASCII
                }

                derive(utf16.data(), utf16.size(), salt, salt_len, key, iv);
            }
    };

    // RAR5 key derivation (PBKDF2-HMAC-SHA256)
    // Password is UTF-8 encoded
    class CRATE_EXPORT rar5_key_derivation {
        public:
            // Derive key, IV, and password check value
            // kdf_count: log2 of iteration count (e.g., 15 = 2^15 = 32768 iterations)
            static void derive(const u8* password, size_t password_len,
                               const u8* salt, size_t salt_len,
                               unsigned kdf_count,
                               u8* key, [[maybe_unused]] u8* iv,
                               u8* pwd_check = nullptr) {
                unsigned iterations = 1u << kdf_count;

                // Derive key (32 bytes)
                pbkdf2_sha256(password, password_len,
                              salt, salt_len,
                              iterations,
                              key, rar5::KEY_SIZE);

                // Derive IV (16 bytes) - from a second PBKDF2 derivation with modified salt
                // In RAR5, the IV is stored in the header, not derived
                // For header encryption, IV is derived from key

                // For password check, RAR5 does iterations+16 more rounds
                if (pwd_check != nullptr) {
                    std::array <u8, rar5::PWD_CHECK_SIZE> check_val{};
                    pbkdf2_sha256(password, password_len,
                                  salt, salt_len,
                                  iterations + 16,
                                  check_val.data(), rar5::PWD_CHECK_SIZE);
                    std::memcpy(pwd_check, check_val.data(), rar5::PWD_CHECK_SIZE);
                }
            }

            // Convenience function with string password
            static void derive(const std::string& password,
                               const u8* salt, size_t salt_len,
                               unsigned kdf_count,
                               u8* key, u8* iv,
                               u8* pwd_check = nullptr) {
                derive(reinterpret_cast <const u8*>(password.data()), password.size(),
                       salt, salt_len, kdf_count, key, iv, pwd_check);
            }

            // Verify password using check value from archive header
            static bool verify_password(const std::string& password,
                                        const u8* salt, size_t salt_len,
                                        unsigned kdf_count,
                                        const u8* expected_check, size_t check_len) {
                std::array <u8, rar5::KEY_SIZE> key{};
                std::array <u8, rar5::IV_SIZE> iv{};
                std::array <u8, rar5::PWD_CHECK_SIZE> check_val{};

                derive(password, salt, salt_len, kdf_count,
                       key.data(), iv.data(), check_val.data());

                // XOR the check bytes together as RAR5 does
                u8 check_xor[rar5::PWD_CHECK_SUM_SIZE] = {0};
                for (size_t i = 0; i < rar5::PWD_CHECK_SIZE; i++) {
                    check_xor[i % rar5::PWD_CHECK_SUM_SIZE] ^= check_val[i];
                }

                // Compare with expected (may be partial)
                return std::memcmp(check_xor, expected_check,
                                   std::min(check_len, size_t(rar5::PWD_CHECK_SUM_SIZE))) == 0;
            }
    };

    // RAR decryption context
    class CRATE_EXPORT rar_decryptor {
        public:
            // Initialize for RAR4 decryption
            void init_rar4(const std::string& password, const u8* salt) {
                std::array <u8, rar4::KEY_SIZE> key{};
                std::array <u8, rar4::IV_SIZE> iv{};

                rar4_key_derivation::derive(password, salt, rar4::SALT_SIZE,
                                            key.data(), iv.data());

                aes_.init_decrypt(key.data(), 128, iv.data());
                block_pos_ = 0;
            }

            // Initialize for RAR5 decryption
            void init_rar5(const std::string& password, const u8* salt,
                           unsigned kdf_count, const u8* iv) {
                std::array <u8, rar5::KEY_SIZE> key{};
                std::array <u8, rar5::IV_SIZE> dummy_iv{};

                rar5_key_derivation::derive(password, salt, rar5::SALT_SIZE,
                                            kdf_count,
                                            key.data(), dummy_iv.data());

                aes_.init_decrypt(key.data(), 256, iv);
                block_pos_ = 0;
            }

            // Decrypt data in-place
            // For RAR, data must be aligned to 16-byte blocks
            void decrypt(u8* data, size_t length) {
                // Ensure block alignment
                size_t aligned_len = length & ~(aes_decoder::BLOCK_SIZE - 1);
                if (aligned_len > 0) {
                    aes_.decrypt_cbc(data, aligned_len);
                }
            }

            // Decrypt and remove PKCS7 padding
            // Returns actual data length after padding removal
            size_t decrypt_final(u8* data, size_t length) {
                if (length == 0 || length % aes_decoder::BLOCK_SIZE != 0) {
                    return length; // Invalid, return as-is
                }

                aes_.decrypt_cbc(data, length);

                // Check and remove PKCS7 padding
                u8 pad_len = data[length - 1];
                if (pad_len > 0 && pad_len <= aes_decoder::BLOCK_SIZE) {
                    // Verify padding bytes
                    bool valid = true;
                    for (size_t i = length - pad_len; i < length; i++) {
                        if (data[i] != pad_len) {
                            valid = false;
                            break;
                        }
                    }
                    if (valid) {
                        return length - pad_len;
                    }
                }

                return length;
            }

        private:
            aes_decoder aes_;
            size_t block_pos_ = 0;
    };
} // namespace crate::crypto
