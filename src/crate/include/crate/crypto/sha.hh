#pragma once

#include <crate/core/types.hh>
#include <array>
#include <cstring>

namespace crate::crypto {
    // SHA-1 implementation for RAR4 key derivation
    class CRATE_EXPORT sha1_hasher {
        public:
            static constexpr size_t DIGEST_SIZE = 20;
            static constexpr size_t BLOCK_SIZE = 64;

            sha1_hasher() { reset(); }

            void reset() {
                state_[0] = 0x67452301;
                state_[1] = 0xefcdab89;
                state_[2] = 0x98badcfe;
                state_[3] = 0x10325476;
                state_[4] = 0xc3d2e1f0;
                count_ = 0;
                buffer_len_ = 0;
            }

            void update(const u8* data, size_t length) {
                while (length > 0) {
                    size_t to_copy = std::min(length, BLOCK_SIZE - buffer_len_);
                    std::memcpy(buffer_ + buffer_len_, data, to_copy);
                    buffer_len_ += to_copy;
                    data += to_copy;
                    length -= to_copy;
                    count_ += to_copy;

                    if (buffer_len_ == BLOCK_SIZE) {
                        process_block(buffer_);
                        buffer_len_ = 0;
                    }
                }
            }

            void finalize(u8* digest) {
                u64 bit_count = count_ * 8;

                // Pad with 0x80 followed by zeros
                u8 pad = 0x80;
                update(&pad, 1);
                pad = 0x00;
                while (buffer_len_ != 56) {
                    update(&pad, 1);
                }

                // Append bit count (big-endian)
                u8 count_bytes[8];
                for (int i = 7; i >= 0; i--) {
                    count_bytes[i] = static_cast <u8>(bit_count);
                    bit_count >>= 8;
                }
                update(count_bytes, 8);

                // Output digest (big-endian)
                for (int i = 0; i < 5; i++) {
                    digest[i * 4 + 0] = static_cast <u8>(state_[i] >> 24);
                    digest[i * 4 + 1] = static_cast <u8>(state_[i] >> 16);
                    digest[i * 4 + 2] = static_cast <u8>(state_[i] >> 8);
                    digest[i * 4 + 3] = static_cast <u8>(state_[i]);
                }
            }

            // Convenience function
            static void hash(const u8* data, size_t length, u8* digest) {
                sha1_hasher ctx;
                ctx.update(data, length);
                ctx.finalize(digest);
            }

        private:
            static constexpr u32 rotl(u32 x, unsigned n) {
                return (x << n) | (x >> (32 - n));
            }

            void process_block(const u8* block) {
                u32 w[80];

                // Load block into w[0..15]
                for (int i = 0; i < 16; i++) {
                    w[i] = (static_cast <u32>(block[i * 4 + 0]) << 24) |
                           (static_cast <u32>(block[i * 4 + 1]) << 16) |
                           (static_cast <u32>(block[i * 4 + 2]) << 8) |
                           (static_cast <u32>(block[i * 4 + 3]));
                }

                // Extend w[16..79]
                for (int i = 16; i < 80; i++) {
                    w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
                }

                u32 a = state_[0];
                u32 b = state_[1];
                u32 c = state_[2];
                u32 d = state_[3];
                u32 e = state_[4];

                for (int i = 0; i < 80; i++) {
                    u32 f, k;
                    if (i < 20) {
                        f = (b & c) | ((~b) & d);
                        k = 0x5a827999;
                    } else if (i < 40) {
                        f = b ^ c ^ d;
                        k = 0x6ed9eba1;
                    } else if (i < 60) {
                        f = (b & c) | (b & d) | (c & d);
                        k = 0x8f1bbcdc;
                    } else {
                        f = b ^ c ^ d;
                        k = 0xca62c1d6;
                    }

                    u32 temp = rotl(a, 5) + f + e + k + w[i];
                    e = d;
                    d = c;
                    c = rotl(b, 30);
                    b = a;
                    a = temp;
                }

                state_[0] += a;
                state_[1] += b;
                state_[2] += c;
                state_[3] += d;
                state_[4] += e;
            }

            u32 state_[5] = {};
            u64 count_ = 0;
            u8 buffer_[BLOCK_SIZE] = {};
            size_t buffer_len_ = 0;
    };

    // SHA-256 implementation for RAR5 key derivation (PBKDF2-HMAC-SHA256)
    class CRATE_EXPORT sha256_hasher {
        public:
            static constexpr size_t DIGEST_SIZE = 32;
            static constexpr size_t BLOCK_SIZE = 64;

            sha256_hasher() { reset(); }

            void reset() {
                state_[0] = 0x6a09e667;
                state_[1] = 0xbb67ae85;
                state_[2] = 0x3c6ef372;
                state_[3] = 0xa54ff53a;
                state_[4] = 0x510e527f;
                state_[5] = 0x9b05688c;
                state_[6] = 0x1f83d9ab;
                state_[7] = 0x5be0cd19;
                count_ = 0;
                buffer_len_ = 0;
            }

            void update(const u8* data, size_t length) {
                while (length > 0) {
                    size_t to_copy = std::min(length, BLOCK_SIZE - buffer_len_);
                    std::memcpy(buffer_ + buffer_len_, data, to_copy);
                    buffer_len_ += to_copy;
                    data += to_copy;
                    length -= to_copy;
                    count_ += to_copy;

                    if (buffer_len_ == BLOCK_SIZE) {
                        process_block(buffer_);
                        buffer_len_ = 0;
                    }
                }
            }

            void finalize(u8* digest) {
                u64 bit_count = count_ * 8;

                // Pad with 0x80 followed by zeros
                u8 pad = 0x80;
                update(&pad, 1);
                pad = 0x00;
                while (buffer_len_ != 56) {
                    update(&pad, 1);
                }

                // Append bit count (big-endian)
                u8 count_bytes[8];
                for (int i = 7; i >= 0; i--) {
                    count_bytes[i] = static_cast <u8>(bit_count);
                    bit_count >>= 8;
                }
                update(count_bytes, 8);

                // Output digest (big-endian)
                for (int i = 0; i < 8; i++) {
                    digest[i * 4 + 0] = static_cast <u8>(state_[i] >> 24);
                    digest[i * 4 + 1] = static_cast <u8>(state_[i] >> 16);
                    digest[i * 4 + 2] = static_cast <u8>(state_[i] >> 8);
                    digest[i * 4 + 3] = static_cast <u8>(state_[i]);
                }
            }

            // Convenience function
            static void hash(const u8* data, size_t length, u8* digest) {
                sha256_hasher ctx;
                ctx.update(data, length);
                ctx.finalize(digest);
            }

        private:
            static constexpr u32 K[64] = {
                0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
                0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
                0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
                0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
                0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
                0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
                0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
                0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
            };

            static constexpr u32 rotr(u32 x, unsigned n) {
                return (x >> n) | (x << (32 - n));
            }

            static constexpr u32 ch(u32 x, u32 y, u32 z) {
                return (x & y) ^ (~x & z);
            }

            static constexpr u32 maj(u32 x, u32 y, u32 z) {
                return (x & y) ^ (x & z) ^ (y & z);
            }

            static constexpr u32 sigma0(u32 x) {
                return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
            }

            static constexpr u32 sigma1(u32 x) {
                return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
            }

            static constexpr u32 gamma0(u32 x) {
                return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
            }

            static constexpr u32 gamma1(u32 x) {
                return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
            }

            void process_block(const u8* block) {
                u32 w[64];

                // Load block into w[0..15]
                for (int i = 0; i < 16; i++) {
                    w[i] = (static_cast <u32>(block[i * 4 + 0]) << 24) |
                           (static_cast <u32>(block[i * 4 + 1]) << 16) |
                           (static_cast <u32>(block[i * 4 + 2]) << 8) |
                           (static_cast <u32>(block[i * 4 + 3]));
                }

                // Extend w[16..63]
                for (int i = 16; i < 64; i++) {
                    w[i] = gamma1(w[i - 2]) + w[i - 7] + gamma0(w[i - 15]) + w[i - 16];
                }

                u32 a = state_[0];
                u32 b = state_[1];
                u32 c = state_[2];
                u32 d = state_[3];
                u32 e = state_[4];
                u32 f = state_[5];
                u32 g = state_[6];
                u32 h = state_[7];

                for (int i = 0; i < 64; i++) {
                    u32 t1 = h + sigma1(e) + ch(e, f, g) + K[i] + w[i];
                    u32 t2 = sigma0(a) + maj(a, b, c);
                    h = g;
                    g = f;
                    f = e;
                    e = d + t1;
                    d = c;
                    c = b;
                    b = a;
                    a = t1 + t2;
                }

                state_[0] += a;
                state_[1] += b;
                state_[2] += c;
                state_[3] += d;
                state_[4] += e;
                state_[5] += f;
                state_[6] += g;
                state_[7] += h;
            }

            u32 state_[8] = {};
            u64 count_ = 0;
            u8 buffer_[BLOCK_SIZE] = {};
            size_t buffer_len_ = 0;
    };

    // HMAC-SHA256 for PBKDF2
    class CRATE_EXPORT hmac_sha256_hasher {
        public:
            static constexpr size_t DIGEST_SIZE = sha256_hasher::DIGEST_SIZE;

            void init(const u8* key, size_t key_len) {
                std::array <u8, sha256_hasher::BLOCK_SIZE> key_block{};

                if (key_len > sha256_hasher::BLOCK_SIZE) {
                    // Hash long keys
                    sha256_hasher::hash(key, key_len, key_block.data());
                } else {
                    std::memcpy(key_block.data(), key, key_len);
                }

                // Compute inner and outer padding
                for (size_t i = 0; i < sha256_hasher::BLOCK_SIZE; i++) {
                    ipad_[i] = key_block[i] ^ 0x36;
                    opad_[i] = key_block[i] ^ 0x5c;
                }

                // Start inner hash
                inner_.reset();
                inner_.update(ipad_.data(), sha256_hasher::BLOCK_SIZE);
            }

            void update(const u8* data, size_t length) {
                inner_.update(data, length);
            }

            void finalize(u8* digest) {
                u8 inner_digest[sha256_hasher::DIGEST_SIZE];
                inner_.finalize(inner_digest);

                // Outer hash
                sha256_hasher outer;
                outer.update(opad_.data(), sha256_hasher::BLOCK_SIZE);
                outer.update(inner_digest, sha256_hasher::DIGEST_SIZE);
                outer.finalize(digest);
            }

            static void compute(const u8* key, size_t key_len,
                                const u8* data, size_t data_len,
                                u8* digest) {
                hmac_sha256_hasher hmac;
                hmac.init(key, key_len);
                hmac.update(data, data_len);
                hmac.finalize(digest);
            }

        private:
            sha256_hasher inner_;
            std::array <u8, sha256_hasher::BLOCK_SIZE> ipad_{};
            std::array <u8, sha256_hasher::BLOCK_SIZE> opad_{};
    };

    // PBKDF2-HMAC-SHA256 for RAR5 key derivation
    inline void pbkdf2_sha256(const u8* password, size_t password_len,
                              const u8* salt, size_t salt_len,
                              unsigned iterations,
                              u8* output, size_t output_len) {
        u32 block_num = 1;

        while (output_len > 0) {
            // U1 = HMAC(password, salt || block_num)
            std::array <u8, sha256_hasher::DIGEST_SIZE> u{};
            std::array <u8, sha256_hasher::DIGEST_SIZE> result{};

            hmac_sha256_hasher hmac;
            hmac.init(password, password_len);
            hmac.update(salt, salt_len);

            // Append block number (big-endian)
            u8 block_bytes[4] = {
                static_cast <u8>(block_num >> 24),
                static_cast <u8>(block_num >> 16),
                static_cast <u8>(block_num >> 8),
                static_cast <u8>(block_num)
            };
            hmac.update(block_bytes, 4);
            hmac.finalize(u.data());

            std::memcpy(result.data(), u.data(), sha256_hasher::DIGEST_SIZE);

            // U2, U3, ... iterations
            for (unsigned i = 1; i < iterations; i++) {
                hmac_sha256_hasher::compute(password, password_len,
                                            u.data(), sha256_hasher::DIGEST_SIZE,
                                            u.data());
                for (size_t j = 0; j < sha256_hasher::DIGEST_SIZE; j++) {
                    result[j] ^= u[j];
                }
            }

            size_t to_copy = std::min(output_len, size_t(sha256_hasher::DIGEST_SIZE));
            std::memcpy(output, result.data(), to_copy);
            output += to_copy;
            output_len -= to_copy;
            block_num++;
        }
    }
} // namespace crate::crypto
