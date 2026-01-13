#pragma once

#include <crate/core/types.hh>
#include <array>

namespace crate {
    namespace detail {
        // CRC-32 table generator (IEEE 802.3 polynomial)
        constexpr u32 CRC32_POLYNOMIAL = 0xEDB88320;

        constexpr std::array <u32, 256> generate_crc32_table() {
            std::array <u32, 256> table{};
            for (u32 i = 0; i < 256; i++) {
                u32 crc = i;
                for (int j = 0; j < 8; j++) {
                    if (crc & 1) {
                        crc = (crc >> 1) ^ CRC32_POLYNOMIAL;
                    } else {
                        crc >>= 1;
                    }
                }
                table[i] = crc;
            }
            return table;
        }

        inline constexpr std::array <u32, 256> CRC32_TABLE = generate_crc32_table();
    }

    // CRC-32 implementation (IEEE 802.3 polynomial)
    class CRATE_EXPORT crc_32 {
        public:
            static constexpr u32 POLYNOMIAL = detail::CRC32_POLYNOMIAL;

            crc_32()
                : value_(0xFFFFFFFF) {
            }

            void reset() {
                value_ = 0xFFFFFFFF;
            }

            void update(u8 value) {
                value_ = detail::CRC32_TABLE[(value_ ^ value) & 0xFF] ^ (value_ >> 8);
            }

            void update(byte_span data) {
                for (u8 value : data) {
                    update(value);
                }
            }

            [[nodiscard]] u32 finalize() const {
                return value_ ^ 0xFFFFFFFF;
            }

            // Get current value without finalizing (for streaming)
            [[nodiscard]] u32 value() const {
                return value_;
            }

        private:
            u32 value_;
    };

    // Convenience function to calculate CRC-32 of a byte span
    inline u32 eval_crc_32(byte_span data) {
        crc_32 crc;
        crc.update(data);
        return crc.finalize();
    }

    // CRC-16 implementation (used by RAR old format checksums)
    // This is NOT a standard CRC - it's a rotate-and-add checksum
    class CRATE_EXPORT crc16_rar_old {
        public:
            crc16_rar_old()
                : value_(0) {
            }

            void reset() {
                value_ = 0;
            }

            void update(u8 value) {
                value_ += value;
                // Rotate left by 1 bit
                value_ = ((value_ & 0x7FFF) << 1) | ((value_ & 0x8000) >> 15);
            }

            void update(byte_span data) {
                for (u8 value : data) {
                    update(value);
                }
            }

            [[nodiscard]] u16 finalize() const {
                return value_;
            }

        private:
            u16 value_;
    };

    /// Convenience function for old RAR checksum
    inline u16 eval_crc16_rar_old(byte_span data) {
        crc16_rar_old crc;
        crc.update(data);
        return crc.finalize();
    }

    // CRC-16-IBM/ANSI implementation (polynomial 0x8005, reflected)
    // Used by: ARC, ZOO, LHA (some variants)
    namespace detail {
        constexpr u16 CRC16_IBM_POLYNOMIAL = 0xA001; // 0x8005 bit-reversed

        constexpr std::array <u16, 256> generate_crc16_ibm_table() {
            std::array <u16, 256> table{};
            for (u32 i = 0; i < 256; i++) {
                u16 crc = static_cast <u16>(i);
                for (int j = 0; j < 8; j++) {
                    if (crc & 1) {
                        crc = (crc >> 1) ^ CRC16_IBM_POLYNOMIAL;
                    } else {
                        crc >>= 1;
                    }
                }
                table[i] = crc;
            }
            return table;
        }

        inline constexpr std::array <u16, 256> CRC16_IBM_TABLE = generate_crc16_ibm_table();
    }

    class CRATE_EXPORT crc_16_ibm {
        public:
            static constexpr u16 POLYNOMIAL = detail::CRC16_IBM_POLYNOMIAL;

            crc_16_ibm()
                : value_(0) {
            }

            void reset() {
                value_ = 0;
            }

            void update(u8 value) {
                value_ = detail::CRC16_IBM_TABLE[(value_ ^ value) & 0xFF] ^ (value_ >> 8);
            }

            void update(byte_span data) {
                for (u8 value : data) {
                    update(value);
                }
            }

            [[nodiscard]] u16 finalize() const {
                return value_;
            }

        private:
            u16 value_;
    };

    /// Convenience function for CRC-16-IBM (used by ARC, ZOO)
    inline u16 eval_crc16_ibm(byte_span data) {
        crc_16_ibm crc;
        crc.update(data);
        return crc.finalize();
    }
} // namespace crate
