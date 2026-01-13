#pragma once

// HYP (Hyper) Archive Format
// Compression methods: Stored, Compressed (adaptive Huffman with dictionary)

#include <memory>
#include <crate/formats/archive.hh>

namespace crate {
    class CRATE_EXPORT hyp_archive : public archive {
        public:
            using archive::extract;
            ~hyp_archive() override;

            static result_t <std::unique_ptr <hyp_archive>> open(byte_span data);

            static result_t <std::unique_ptr <hyp_archive>> open(const std::filesystem::path& path);

            [[nodiscard]] const std::vector <file_entry>& files() const override;

            result_t <byte_vector> extract(const file_entry& entry) override;

        private:
            hyp_archive();

            void_result_t parse();

            struct impl;
            std::unique_ptr <impl> m_pimpl;
    };
} // namespace crate
