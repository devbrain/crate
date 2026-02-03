#pragma once

#include <crate/core/types.hh>
#include <crate/core/error.hh>
#include <fstream>
#include <filesystem>
#include <memory>
#include <concepts>
#include <algorithm>

namespace crate {
    // Input stream concept
    template<typename T>
    concept InputStream = requires(T stream, mutable_byte_span buffer)
    {
        { stream.read(buffer) } -> std::same_as <result_t <size_t>>;
        { stream.seek(size_t{}) } -> std::same_as <void_result_t>;
        { stream.tell() } -> std::same_as <result_t <size_t>>;
        { stream.size() } -> std::same_as <result_t <size_t>>;
    };

    // Output stream concept
    template<typename T>
    concept OutputStream = requires(T stream, byte_span data)
    {
        { stream.write(data) } -> std::same_as <void_result_t>;
    };

    // Output stream base interface for streaming extraction
    class CRATE_EXPORT output_stream {
        public:
            virtual ~output_stream() = default;
            virtual void_result_t write(byte_span data) = 0;
    };

    // Memory input stream - wraps a span of bytes
    class CRATE_EXPORT memory_input_stream {
        public:
            explicit memory_input_stream(byte_span data)
                : data_(data), pos_(0) {
            }

            result_t <size_t> read(mutable_byte_span buffer) {
                size_t available = data_.size() - pos_;
                size_t to_read = std::min(buffer.size(), available);
                std::copy_n(data_.data() + pos_, to_read, buffer.data());
                pos_ += to_read;
                return to_read;
            }

            void_result_t seek(size_t pos) {
                if (pos > data_.size()) {
                    return std::unexpected(error{error_code::SeekError, "Seek past end of buffer"});
                }
                pos_ = pos;
                return {};
            }

            [[nodiscard]] result_t <size_t> tell() const { return pos_; }
            [[nodiscard]] result_t <size_t> size() const { return data_.size(); }

            [[nodiscard]] byte_span remaining() const { return data_.subspan(pos_); }

        private:
            byte_span data_;
            size_t pos_;
    };

    // File input stream
    class CRATE_EXPORT file_input_stream {
        public:
            static result_t <file_input_stream> open(const std::filesystem::path& path) {
                std::ifstream file(path, std::ios::binary);
                if (!file) {
                    return std::unexpected(error{
                        error_code::FileNotFound,
                        "Failed to open: " + path.string()
                    });
                }

                file.seekg(0, std::ios::end);
                auto pos = file.tellg();
                if (pos < 0) {
                    return std::unexpected(error{
                        error_code::ReadError,
                        "Failed to determine file size: " + path.string()
                    });
                }
                size_t size = static_cast<size_t>(pos);
                file.seekg(0, std::ios::beg);

                return file_input_stream(std::move(file), size);
            }

            result_t <size_t> read(mutable_byte_span buffer) {
                file_.read(reinterpret_cast <char*>(buffer.data()),
                           static_cast <std::streamsize>(buffer.size()));
                if (file_.bad()) {
                    return std::unexpected(error{error_code::ReadError});
                }
                return static_cast <size_t>(file_.gcount());
            }

            void_result_t seek(size_t pos) {
                file_.seekg(static_cast <std::streamoff>(pos));
                if (file_.fail()) {
                    return std::unexpected(error{error_code::SeekError});
                }
                return {};
            }

            [[nodiscard]] result_t<size_t> tell() const {
                auto pos = file_.tellg();
                if (pos < 0) return std::unexpected(error{error_code::SeekError});
                return static_cast<size_t>(pos);
            }

            [[nodiscard]] result_t<size_t> size() const { return size_; }

        private:
            file_input_stream(std::ifstream file, size_t size)
                : file_(std::move(file)), size_(size) {
            }

            mutable std::ifstream file_;
            size_t size_;
    };

    // Vector output stream
    class CRATE_EXPORT vector_output_stream : public output_stream {
        public:
            vector_output_stream() = default;
            explicit vector_output_stream(size_t reserve) { data_.reserve(reserve); }

            void_result_t write(byte_span data) override {
                data_.insert(data_.end(), data.begin(), data.end());
                return {};
            }

            [[nodiscard]] const byte_vector& data() const { return data_; }
            [[nodiscard]] byte_vector&& take() { return std::move(data_); }

        private:
            byte_vector data_;
    };

    // File output stream
    class CRATE_EXPORT file_output_stream : public output_stream {
        public:
            static result_t <file_output_stream> create(const std::filesystem::path& path) {
                // Create parent directories if needed
                if (auto parent = path.parent_path(); !parent.empty()) {
                    std::error_code ec;
                    std::filesystem::create_directories(parent, ec);
                }

                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                if (!file) {
                    return std::unexpected(error{
                        error_code::WriteError,
                        "Failed to create: " + path.string()
                    });
                }
                return file_output_stream(std::move(file));
            }

            void_result_t write(byte_span data) override {
                file_.write(reinterpret_cast <const char*>(data.data()),
                            static_cast <std::streamsize>(data.size()));
                if (file_.fail()) {
                    return std::unexpected(error{error_code::WriteError});
                }
                return {};
            }

        private:
            explicit file_output_stream(std::ofstream file)
                : file_(std::move(file)) {
            }

            std::ofstream file_;
    };
} // namespace crate
