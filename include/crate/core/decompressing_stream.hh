#pragma once

#include <crate/core/decompressor.hh>
#include <crate/core/system.hh>
#include <istream>
#include <memory>
#include <vector>

namespace crate {

// Custom streambuf that decompresses data on-demand from a source stream
class CRATE_EXPORT decompressing_streambuf : public std::streambuf {
public:
    // Construct with source stream and decompressor
    // @param source: Input stream containing compressed data
    // @param decompressor: Decompressor instance
    // @param input_buffer_size: Size of internal compressed data buffer
    // @param output_buffer_size: Size of internal decompressed data buffer
    decompressing_streambuf(
        std::istream& source,
        std::unique_ptr<decompressor> decomp,
        size_t input_buffer_size = 8192,
        size_t output_buffer_size = 8192
    );

    ~decompressing_streambuf() override;

    // Non-copyable
    decompressing_streambuf(const decompressing_streambuf&) = delete;
    decompressing_streambuf& operator=(const decompressing_streambuf&) = delete;

    // Movable
    decompressing_streambuf(decompressing_streambuf&&) noexcept;
    decompressing_streambuf& operator=(decompressing_streambuf&&) noexcept;

    // Check if an error occurred during decompression
    [[nodiscard]] bool has_error() const { return has_error_; }
    [[nodiscard]] const error& last_error() const { return *last_error_; }

protected:
    // Called when input buffer is exhausted
    int_type underflow() override;

private:
    // Read more compressed data from source into input buffer
    bool fill_input_buffer();

    // Decompress data into output buffer
    bool decompress_to_output();

    std::istream* source_;
    std::unique_ptr<decompressor> decompressor_;

    byte_vector input_buffer_;
    byte_vector output_buffer_;

    size_t input_pos_ = 0;      // Current position in input buffer
    size_t input_size_ = 0;     // Valid bytes in input buffer

    bool source_exhausted_ = false;
    bool decompression_finished_ = false;
    bool has_error_ = false;
    std::optional<error> last_error_;
};

// istream that reads decompressed data from a compressed source stream
class CRATE_EXPORT idecompressing_stream : public std::istream {
public:
    // Construct with source stream and decompressor
    idecompressing_stream(
        std::istream& source,
        std::unique_ptr<decompressor> decomp,
        size_t input_buffer_size = 8192,
        size_t output_buffer_size = 8192
    );

    ~idecompressing_stream() override;

    // Non-copyable
    idecompressing_stream(const idecompressing_stream&) = delete;
    idecompressing_stream& operator=(const idecompressing_stream&) = delete;

    // Movable
    idecompressing_stream(idecompressing_stream&&) noexcept;
    idecompressing_stream& operator=(idecompressing_stream&&) noexcept;

    // Check if a decompression error occurred
    [[nodiscard]] bool has_decompression_error() const;
    [[nodiscard]] const error& decompression_error() const;

private:
    std::unique_ptr<decompressing_streambuf> buf_;
};

} // namespace crate
