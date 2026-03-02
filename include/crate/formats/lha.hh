#pragma once

// LHA/LZH Archive Format
// Compression methods: LH0 (stored), LH1-LH7, LZS, LZ4, LZ5, PM0-PM2

#include <memory>
#include <crate/formats/archive.hh>

namespace crate {
    class CRATE_EXPORT lha_archive : public archive {
        public:
            using archive::extract;
            ~lha_archive() override;

            static result_t<std::unique_ptr<lha_archive>> open(byte_span data);

            static result_t<std::unique_ptr<lha_archive>> open(const std::filesystem::path& path);

            static result_t<std::unique_ptr<lha_archive>> open(std::istream& stream);

            [[nodiscard]] const std::vector<file_entry>& files() const override;

            result_t<byte_vector> extract(const file_entry& entry) override;

        private:
            lha_archive();

            void_result_t parse();

            struct impl;
            std::unique_ptr<impl> m_pimpl;
    };
} // namespace crate
