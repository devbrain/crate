#pragma once

#include <crate/core/types.hh>
#include <crate/core/error.hh>
#include <crate/core/system.hh>
#include <functional>

namespace crate {

// Status of a streaming decompression operation (follows brotli pattern)
enum class decode_status : u8 {
    done,              // Decompression finished successfully
    needs_more_input,  // Need more input data to continue
    needs_more_output, // Output buffer full, need more space
};

// Result of a streaming decompression operation
struct stream_result {
    size_t bytes_read;      // Bytes consumed from input
    size_t bytes_written;   // Bytes written to output
    decode_status status;   // Current decoder status

    // Convenience accessor for compatibility
    [[nodiscard]] bool finished() const { return status == decode_status::done; }

    // Helper constructors for common cases
    static stream_result done(size_t read, size_t written) {
        return {read, written, decode_status::done};
    }
    static stream_result need_input(size_t read, size_t written) {
        return {read, written, decode_status::needs_more_input};
    }
    static stream_result need_output(size_t read, size_t written) {
        return {read, written, decode_status::needs_more_output};
    }
};

// Abstract decompressor interface with streaming as the core API
class CRATE_EXPORT decompressor {
public:
    virtual ~decompressor();

    // Progress callback: (bytes_written, total_expected)
    // total_expected may be 0 if unknown
    using progress_callback_t = std::function<void(size_t bytes_written, size_t total_expected)>;

    // Set progress callback for byte-level progress reporting
    void set_progress_callback(progress_callback_t cb);

    // =========================================================================
    // Core streaming interface (must be implemented by derived classes)
    // =========================================================================

    // Decompress incrementally
    // @param input: Compressed input data (may be partial)
    // @param output: Buffer to write decompressed data
    // @param input_finished: True if this is the last input chunk
    // @return stream_result with bytes consumed/written and completion status
    [[nodiscard]] virtual result_t<stream_result> decompress_some(
        byte_span input,
        mutable_byte_span output,
        bool input_finished = false
    ) = 0;

    // Reset decompressor state for reuse
    virtual void reset() = 0;

    // =========================================================================
    // Convenience one-shot interface (implemented in base class)
    // =========================================================================

    // Decompress all data from input to output in one call
    // This is a wrapper around decompress_some() for simple use cases
    // @param input: Complete compressed input data
    // @param output: Pre-sized buffer for decompressed output
    // @return Number of bytes written to output
    [[nodiscard]] result_t<size_t> decompress(byte_span input, mutable_byte_span output);

protected:
    // Call this from derived classes to report progress
    void report_progress(size_t bytes_written, size_t total_expected = 0) const;

    progress_callback_t progress_cb_;
};

// Factory function type
using decompressor_factory = std::unique_ptr<decompressor>(*)();

} // namespace crate
