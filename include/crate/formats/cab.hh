#pragma once

#include <crate/formats/archive.hh>

namespace crate {
    class CRATE_EXPORT cab_archive : public archive {
        public:
            using archive::extract;

            // Destructor (required for PIMPL)
            ~cab_archive() override;

            static result_t <std::unique_ptr <cab_archive>> open(byte_span data);

            static result_t <std::unique_ptr <cab_archive>> open(const std::filesystem::path& path);

            const std::vector <file_entry>& files() const override;

            result_t <byte_vector> extract(const file_entry& entry) override;

        private:
            cab_archive();

            void_result_t parse();

            void_result_t parse_header();

            void_result_t parse_folders();

            void_result_t parse_files();

            result_t <byte_vector> decompress_folder(u32 folder_index, size_t max_size);

            // PIMPL for internal implementation details
            struct impl;
            mutable std::unique_ptr <impl> pimpl_;
    };
} // namespace crate
