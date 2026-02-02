#include <crate/core/decompressing_stream.hh>
#include <cstring>

namespace crate {

// ============================================================================
// decompressing_streambuf
// ============================================================================

decompressing_streambuf::decompressing_streambuf(
    std::istream& source,
    std::unique_ptr<decompressor> decomp,
    size_t input_buffer_size,
    size_t output_buffer_size,
    size_t expected_output_size
)
    : source_(&source),
      decompressor_(std::move(decomp)),
      input_buffer_(input_buffer_size),
      output_buffer_(output_buffer_size) {
    if (expected_output_size > 0 && decompressor_ && decompressor_->requires_output_size()) {
        decompressor_->set_expected_output_size(expected_output_size);
    }
    // Initialize with empty buffer - underflow will fill it
    setg(nullptr, nullptr, nullptr);
}

decompressing_streambuf::~decompressing_streambuf() = default;

decompressing_streambuf::decompressing_streambuf(decompressing_streambuf&&) noexcept = default;
decompressing_streambuf& decompressing_streambuf::operator=(decompressing_streambuf&&) noexcept = default;

bool decompressing_streambuf::fill_input_buffer() {
    if (source_exhausted_) {
        return input_size_ > input_pos_;  // Still have buffered data
    }

    // Move any remaining data to the beginning
    if (input_pos_ > 0 && input_size_ > input_pos_) {
        size_t remaining = input_size_ - input_pos_;
        std::memmove(input_buffer_.data(), input_buffer_.data() + input_pos_, remaining);
        input_size_ = remaining;
        input_pos_ = 0;
    } else {
        input_size_ = 0;
        input_pos_ = 0;
    }

    // Read more data from source
    size_t space = input_buffer_.size() - input_size_;
    if (space > 0) {
        source_->read(reinterpret_cast<char*>(input_buffer_.data() + input_size_), 
                      static_cast<std::streamsize>(space));
        auto read_count = source_->gcount();
        input_size_ += static_cast<size_t>(read_count);

        if (source_->eof() || read_count == 0) {
            source_exhausted_ = true;
        }
    }

    return input_size_ > input_pos_;
}

bool decompressing_streambuf::decompress_to_output() {
    if (decompression_finished_) {
        return false;
    }

    // Fill input buffer if needed
    if (input_pos_ >= input_size_) {
        if (!fill_input_buffer()) {
            // No more input
            if (!source_exhausted_) {
                return false;  // Need more input
            }
        }
    }

    // Prepare input/output spans (byte_vector uses byte = uint8_t)
    byte_span input{input_buffer_.data() + input_pos_, input_size_ - input_pos_};
    mutable_byte_span output{output_buffer_.data(), output_buffer_.size()};

    // Decompress
    auto result = decompressor_->decompress_some(input, output, source_exhausted_);
    if (!result) {
        has_error_ = true;
        last_error_ = result.error();
        return false;
    }

    // Update positions
    input_pos_ += result->bytes_read;

    // Set up get area with decompressed data
    if (result->bytes_written > 0) {
        char* base = reinterpret_cast<char*>(output_buffer_.data());
        setg(base, base, base + result->bytes_written);
    }

    if (result->finished()) {
        decompression_finished_ = true;
    }

    return result->bytes_written > 0;
}

decompressing_streambuf::int_type decompressing_streambuf::underflow() {
    // If we still have data in the buffer, return it
    if (gptr() < egptr()) {
        return traits_type::to_int_type(*gptr());
    }

    // Try to decompress more data
    if (decompress_to_output()) {
        return traits_type::to_int_type(*gptr());
    }

    // End of stream or error
    return traits_type::eof();
}

// ============================================================================
// idecompressing_stream
// ============================================================================

idecompressing_stream::idecompressing_stream(
    std::istream& source,
    std::unique_ptr<decompressor> decomp,
    size_t input_buffer_size,
    size_t output_buffer_size,
    size_t expected_output_size
)
    : std::istream(nullptr),
      buf_(std::make_unique<decompressing_streambuf>(
          source, std::move(decomp), input_buffer_size, output_buffer_size, expected_output_size)) {
    rdbuf(buf_.get());
}

idecompressing_stream::~idecompressing_stream() = default;

idecompressing_stream::idecompressing_stream(idecompressing_stream&& other) noexcept
    : std::istream(std::move(other)),
      buf_(std::move(other.buf_)) {
    set_rdbuf(buf_.get());
}

idecompressing_stream& idecompressing_stream::operator=(idecompressing_stream&& other) noexcept {
    if (this != &other) {
        std::istream::operator=(std::move(other));
        buf_ = std::move(other.buf_);
        set_rdbuf(buf_.get());
    }
    return *this;
}

bool idecompressing_stream::has_decompression_error() const {
    return buf_->has_error();
}

const error& idecompressing_stream::decompression_error() const {
    return buf_->last_error();
}

} // namespace crate
