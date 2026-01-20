#include <crate/core/decompressor.hh>

namespace crate {

decompressor::~decompressor() = default;

void decompressor::set_progress_callback(progress_callback_t cb) {
    progress_cb_ = std::move(cb);
}

void decompressor::report_progress(size_t bytes_written, size_t total_expected) const {
    if (progress_cb_) {
        progress_cb_(bytes_written, total_expected);
    }
}

result_t<size_t> decompressor::decompress(byte_span input, mutable_byte_span output) {
    size_t total_read = 0;
    size_t total_written = 0;
    bool bounded = requires_output_size();

    if (bounded) {
        set_expected_output_size(output.size());
    }

    auto clear_expected = [&]() {
        if (bounded) {
            clear_expected_output_size();
        }
    };

    while (total_read < input.size() || total_written == 0) {
        auto remaining_input = input.subspan(total_read);
        auto remaining_output = output.subspan(total_written);

        if (remaining_output.empty()) {
            clear_expected();
            return std::unexpected(error{error_code::OutputBufferOverflow, "Output buffer too small"});
        }

        auto result = decompress_some(remaining_input, remaining_output, true);
        if (!result) {
            clear_expected();
            return std::unexpected(result.error());
        }

        total_read += result->bytes_read;
        total_written += result->bytes_written;

        if (result->finished()) {
            break;
        }

        // If no progress was made, we're stuck
        if (result->bytes_read == 0 && result->bytes_written == 0) {
            clear_expected();
            return std::unexpected(error{error_code::CorruptData, "Decompression stalled"});
        }
    }

    clear_expected();
    return total_written;
}

} // namespace crate
