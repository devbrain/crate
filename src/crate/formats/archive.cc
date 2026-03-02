#include <crate/formats/archive.hh>

namespace crate {
    bool file_entry::directory() const { return is_directory; }

    bool file_entry::encrypted() const { return is_encrypted; }

    double file_entry::compression_ratio() const {
        if (uncompressed_size == 0) return 0.0;
        return 1.0 - (static_cast <double>(compressed_size) / static_cast <double>(uncompressed_size));
    }

    int file_entry::compression_percent() const {
        return static_cast <int>(compression_ratio() * 100.0);
    }

    result_t <size_t> archive::extract_to(const file_entry& entry, output_stream& dest) {
        auto data = extract(entry);
        if (!data) return crate::make_unexpected(data.error());

        auto write = dest.write(*data);
        if (!write) return crate::make_unexpected(write.error());

        return data->size();
    }

    result_t<std::unique_ptr<std::istream>>
    archive::extract_stream(const file_entry& entry) {
        auto data = extract(entry);
        if (!data) return crate::make_unexpected(data.error());
        return std::make_unique<byte_vector_istream>(std::move(*data));
    }

    void_result_t archive::extract(const file_entry& entry, const std::filesystem::path& dest) {
        auto output = file_output_stream::create(dest);
        if (!output) return crate::make_unexpected(output.error());

        auto result = extract_to(entry, *output);
        if (!result) return crate::make_unexpected(result.error());

        return {};
    }

    void_result_t archive::extract_all(const std::filesystem::path& dest_dir) {
        const auto& file_list = files();
        for (size_t i = 0; i < file_list.size(); i++) {
            const auto& entry = file_list[i];

            if (progress_cb_) {
                progress_cb_(entry, i + 1, file_list.size());
            }

            auto dest = dest_dir / entry.name;

            // Create parent directories
            auto parent = dest.parent_path();
            if (!parent.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(parent, ec);
                // Ignore errors - extract will fail if dir creation failed
            }

            auto result = extract(entry, dest);
            if (!result) return result;
        }
        return {};
    }

    void archive::set_progress_callback(progress_callback_t cb) { progress_cb_ = std::move(cb); }

    void archive::set_byte_progress_callback(byte_progress_callback_t cb) { byte_progress_cb_ = std::move(cb); }
} // namespace crate
