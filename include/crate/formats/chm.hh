#pragma once

#include <memory>
#include <crate/formats/archive.hh>

namespace crate {
    class CRATE_EXPORT chm_archive : public archive {
        public:
            using archive::extract;

            ~chm_archive() override;

            static result_t <std::unique_ptr <chm_archive>> open(byte_span data);

            static result_t <std::unique_ptr <chm_archive>> open(const std::filesystem::path& path);

            [[nodiscard]] const std::vector <file_entry>& files() const override;

            result_t <byte_vector> extract(const file_entry& entry) override;

            void_result_t extract(const file_entry& entry, const std::filesystem::path& dest) override;

            void_result_t extract_all(const std::filesystem::path& dest_dir) override;

            // Accessors
            //[[nodiscard]] const chm::itsf_header& itsf_header() const { return itsf_; }
            //[[nodiscard]] const chm::itsp_header& itsp_header() const { return itsp_; }

        private:
            chm_archive();

            void_result_t parse();

            void_result_t parse_itsf_header();

            static result_t <u64> read_encint(byte_span data, size_t& pos);

            void_result_t parse_directory();

            void_result_t parse_reset_table();

            void_result_t decompress_content();

            struct impl;
            std::unique_ptr <impl> m_pimpl;
    };
} // namespace crate
