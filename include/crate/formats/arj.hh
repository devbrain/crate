#pragma once

#include <memory>
#include <crate/formats/archive.hh>

namespace crate {

    class CRATE_EXPORT arj_archive : public archive {
        public:
            using archive::extract;
            ~arj_archive() override;

            static result_t <std::unique_ptr <arj_archive>> open(byte_span data);

            static result_t <std::unique_ptr <arj_archive>> open(const std::filesystem::path& path);

            [[nodiscard]] const std::vector <file_entry>& files() const override;

            result_t <byte_vector> extract(const file_entry& entry) override;

            // Accessors
            // [[nodiscard]] const arj::main_header& main_header() const;
            // [[nodiscard]] const std::vector <arj::member_header>& members() const;

        private:
            arj_archive();

            void_result_t parse();

            result_t <size_t> find_and_parse_main_header(size_t pos);

            result_t <size_t> parse_member(size_t pos);

            [[nodiscard]] result_t <size_t> skip_extended_headers(size_t pos) const;

            struct impl;
            std::unique_ptr<impl> m_pimpl;


    };
} // namespace crate
