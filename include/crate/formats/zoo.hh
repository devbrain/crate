#pragma once

// ZOO Archive Format (1985)
// Compression methods: Stored, LZW, LH5

#include <memory>
#include <crate/formats/archive.hh>

namespace crate {
    class CRATE_EXPORT zoo_archive : public archive {
        public:
            using archive::extract;
            ~zoo_archive() override;

            static result_t <std::unique_ptr <zoo_archive>> open(byte_span data);

            static result_t <std::unique_ptr <zoo_archive>> open(const std::filesystem::path& path);

            static result_t <std::unique_ptr <zoo_archive>> open(std::istream& stream);

            [[nodiscard]] const std::vector <file_entry>& files() const override;

            result_t <byte_vector> extract(const file_entry& entry) override;
            result_t <size_t> extract_to(const file_entry& entry, output_stream& dest) override;

        private:
            zoo_archive();

            void_result_t parse();

            struct impl;
            std::unique_ptr <impl> m_pimpl;
    };
} // namespace crate
