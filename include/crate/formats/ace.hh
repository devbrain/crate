#pragma once

// ACE Archive Format
// Compression methods: Stored, LZ77 (proprietary)
// Based on acefile (https://github.com/droe/acefile)

#include <memory>
#include <crate/formats/archive.hh>


namespace crate {
    class CRATE_EXPORT ace_archive : public archive {
        public:
            ~ace_archive() override;

            using archive::extract;
            static result_t <std::unique_ptr <ace_archive>> open(byte_span data);

            static result_t <std::unique_ptr <ace_archive>>
            open(const std::filesystem::path& path);

            [[nodiscard]] const std::vector <file_entry>& files() const override;

            result_t <byte_vector> extract(const file_entry& entry) override;

            //[[nodiscard]] const ace::main_header& main_header() const { return
            //main_header_; }
            //[[nodiscard]] const std::vector <ace::file_header>& members() const { return
            //members_; }

        private:
            ace_archive();

            void_result_t parse();

            size_t find_signature() const;

            void_result_t parse_main_header(size_t& pos);

            void_result_t parse_file_header(size_t& pos);

            struct impl;
            std::unique_ptr<impl> m_pimpl;


    };
} // namespace crate
