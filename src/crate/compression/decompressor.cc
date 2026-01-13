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
} // namespace crate
