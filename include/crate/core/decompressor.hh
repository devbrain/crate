#pragma once

#include <crate/core/types.hh>
#include <crate/core/error.hh>
#include <crate/core/system.hh>
#include <functional>
#include <array>
#include <cstring>

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

    // True streaming support (partial input/output across calls)
    [[nodiscard]] virtual bool supports_streaming() const { return false; }

    // =========================================================================
    // Convenience one-shot interface (implemented in base class)
    // =========================================================================

    // Decompress all data from input to output in one call
    // This is a wrapper around decompress_some() for simple use cases
    // @param input: Complete compressed input data
    // @param output: Pre-sized buffer for decompressed output
    // @return Number of bytes written to output
    [[nodiscard]] result_t<size_t> decompress(byte_span input, mutable_byte_span output);

    // Decompress from a stream into an output stream.
    // Uses true streaming when supported, otherwise buffers the full input.
    template<InputStream Input>
    [[nodiscard]] result_t<size_t> decompress_stream(
        Input& input,
        output_stream& output,
        size_t expected_size = 0
    ) {
        if (!supports_streaming()) {
            byte_vector compressed;
            if (auto size = input.size(); size) {
                compressed.reserve(*size);
            }

            std::array<byte, 64 * 1024> buffer{};
            while (true) {
                auto read = input.read(mutable_byte_span{buffer.data(), buffer.size()});
                if (!read) {
                    return std::unexpected(read.error());
                }
                if (*read == 0) {
                    break;
                }
                compressed.insert(compressed.end(), buffer.data(), buffer.data() + *read);
            }

            if (compressed.empty() && expected_size == 0) {
                return 0;
            }
            if (expected_size == 0) {
                return std::unexpected(error{
                    error_code::InvalidParameter,
                    "Expected size required for buffered decompression"
                });
            }

            byte_vector decompressed(expected_size);
            auto result = decompress(compressed, decompressed);
            if (!result) {
                return std::unexpected(result.error());
            }
            decompressed.resize(*result);

            auto write = output.write(decompressed);
            if (!write) {
                return std::unexpected(write.error());
            }
            return *result;
        }

        constexpr size_t input_buffer_size = 64 * 1024;
        constexpr size_t output_buffer_size = 64 * 1024;

        byte_vector input_buffer(input_buffer_size);
        byte_vector output_buffer(output_buffer_size);
        size_t input_pos = 0;
        size_t input_size = 0;
        bool input_exhausted = false;
        size_t total_written = 0;

        auto fill_input = [&]() -> void_result_t {
            if (input_exhausted) {
                return {};
            }

            if (input_pos > 0 && input_pos < input_size) {
                size_t remaining = input_size - input_pos;
                std::memmove(input_buffer.data(), input_buffer.data() + input_pos, remaining);
                input_size = remaining;
                input_pos = 0;
            } else if (input_pos >= input_size) {
                input_pos = 0;
                input_size = 0;
            }

            size_t space = input_buffer.size() - input_size;
            if (space == 0) {
                return {};
            }

            auto read = input.read(mutable_byte_span{input_buffer.data() + input_size, space});
            if (!read) {
                return std::unexpected(read.error());
            }
            if (*read == 0) {
                input_exhausted = true;
                return {};
            }

            input_size += *read;
            return {};
        };

        while (true) {
            if (!input_exhausted && input_pos >= input_size) {
                auto fill = fill_input();
                if (!fill) {
                    return std::unexpected(fill.error());
                }
            }

            byte_span in_span{input_buffer.data() + input_pos, input_size - input_pos};
            bool input_finished = input_exhausted && input_pos >= input_size;
            mutable_byte_span out_span{output_buffer.data(), output_buffer.size()};

            auto result = decompress_some(in_span, out_span, input_finished);
            if (!result) {
                return std::unexpected(result.error());
            }

            input_pos += result->bytes_read;
            if (result->bytes_written > 0) {
                auto write = output.write(byte_span{output_buffer.data(), result->bytes_written});
                if (!write) {
                    return std::unexpected(write.error());
                }
                total_written += result->bytes_written;
            }

            if (result->finished()) {
                break;
            }

            if (result->status == decode_status::needs_more_input && !input_finished) {
                auto fill = fill_input();
                if (!fill) {
                    return std::unexpected(fill.error());
                }
            }

            if (result->bytes_read == 0 && result->bytes_written == 0) {
                if (input_finished) {
                    return std::unexpected(error{
                        error_code::CorruptData,
                        "Decompression stalled at end of input"
                    });
                }
            }
        }

        return total_written;
    }

protected:
    // Call this from derived classes to report progress
    void report_progress(size_t bytes_written, size_t total_expected = 0) const;

    progress_callback_t progress_cb_;
};

// Factory function type
using decompressor_factory = std::unique_ptr<decompressor>(*)();

} // namespace crate
