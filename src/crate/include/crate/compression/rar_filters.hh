#pragma once

#include <crate/core/types.hh>
#include <vector>
#include <cstring>

namespace crate {
    // RAR5 filter types
    enum class rar_filter_type : u8 {
        DELTA = 0, // Delta encoding (for audio/image data)
        E8 = 1, // x86 E8 (CALL) instruction filter
        E8E9 = 2, // x86 E8/E9 (CALL/JMP) instruction filter
        ARM = 3, // ARM BL instruction filter
    };

    // A single filter to be applied to decompressed data
    struct CRATE_EXPORT rar_filter {
        rar_filter_type type = rar_filter_type::DELTA;
        u64 block_start = 0; // Start position in output
        u64 block_length = 0; // Length of data to filter
        u8 channels = 0; // Number of channels (for DELTA filter)
    };

    // RAR5 filter processor
    class CRATE_EXPORT rar5_filter_processor {
        public:
            // Add a filter to be applied
            void add_filter(const rar_filter& filter);

            // Clear all filters
            void clear();

            // Apply all filters to data at given file position
            // Returns true if any filters were applied
            bool apply_filters(u8* data, size_t size, u64 file_pos) const;

        private:
            // DELTA filter: reverse delta encoding
            // Data is encoded as differences between adjacent values per channel
            static void apply_delta_filter(u8* data, size_t size, u8 channels);

            // E8 filter: reverse x86 CALL instruction address transformation
            // CALL instructions (E8 xx xx xx xx) have relative addresses
            // converted to absolute during compression
            static void apply_e8_filter(u8* data, size_t size, u64 file_pos);

            // E8E9 filter: reverse x86 CALL/JMP instruction address transformation
            // Both E8 (CALL) and E9 (JMP) instructions are processed
            static void apply_e8e9_filter(u8* data, size_t size, u64 file_pos);

            // ARM filter: reverse ARM BL (Branch with Link) instruction transformation
            // ARM BL instructions are 4 bytes: EB xx xx xx (condition "always")
            // The offset is a 24-bit signed value shifted left by 2
            static void apply_arm_filter(u8* data, size_t size, u64 file_pos);

            std::vector <rar_filter> filters_;
    };

    // RAR3/4 VM filter types (for reference, not fully implemented)
    // These use a virtual machine for complex transformations
    enum class rar3_filter_type : u8 {
        VMSF_NONE = 0,
        VMSF_E8 = 1,
        VMSF_E8E9 = 2,
        VMSF_ITANIUM = 3,
        VMSF_RGB = 4,
        VMSF_AUDIO = 5,
        VMSF_DELTA = 6,
        VMSF_UPCASE = 7,
    };
} // namespace crate
