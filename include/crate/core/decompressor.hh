#pragma once

#include <crate/core/types.hh>
#include <crate/core/error.hh>
#include <crate/core/system.hh>
#include <functional>

namespace crate {

// Abstract decompressor interface
class CRATE_EXPORT decompressor {
public:
    virtual ~decompressor();

    // Progress callback: (bytes_written, total_expected)
    // total_expected may be 0 if unknown
    using progress_callback_t = std::function<void(size_t bytes_written, size_t total_expected)>;

    // Set progress callback for byte-level progress reporting
    void set_progress_callback(progress_callback_t cb);

    // Decompress data from input to output
    // Returns number of bytes written to output
    [[nodiscard]] virtual result_t<size_t> decompress(
        byte_span input,
        mutable_byte_span output
    ) = 0;

    // Reset decompressor state (for multi-block decompression)
    virtual void reset() = 0;

protected:
    // Call this from derived classes to report progress
    void report_progress(size_t bytes_written, size_t total_expected = 0) const;

    progress_callback_t progress_cb_;
};

// Factory function type
using decompressor_factory = std::unique_ptr<decompressor>(*)();

} // namespace crate
