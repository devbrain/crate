#pragma once

// ARC Archive Format - SEA ARC (1985)
// Compression methods: Stored, RLE90, Squeezed (Huffman), Crunched/Squashed (LZW)

#include <memory>
#include <crate/formats/archive.hh>



namespace crate {


    class CRATE_EXPORT arc_archive : public archive {
        public:
            using archive::extract;
            ~arc_archive() override;

            static result_t <std::unique_ptr <arc_archive>> open(byte_span data);

            static result_t <std::unique_ptr <arc_archive>> open(const std::filesystem::path& path);

            static result_t <std::unique_ptr <arc_archive>> open(std::istream& stream);

            [[nodiscard]] const std::vector <file_entry>& files() const override;

            result_t <byte_vector> extract(const file_entry& entry) override;

      //      [[nodiscard]] const std::vector <arc::member_header>& members() const;

        private:
            arc_archive();

            void_result_t parse();

            struct impl;

            std::unique_ptr<impl> m_pimpl;

    };
} // namespace crate
