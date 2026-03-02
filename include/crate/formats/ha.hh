#pragma once

// HA Archive Format
// Compression methods: CPY (stored), ASC (LZ77+arithmetic), HSC (PPM+arithmetic)
// Based on unarc-rs implementation

#include <memory>
#include <crate/formats/archive.hh>

namespace crate {
    class CRATE_EXPORT ha_archive : public archive {
        public:
            using archive::extract;
            ~ha_archive() override;

            static result_t <std::unique_ptr <ha_archive>> open(byte_span data);

            static result_t <std::unique_ptr <ha_archive>> open(const std::filesystem::path& path);

            static result_t <std::unique_ptr <ha_archive>> open(std::istream& stream);

            [[nodiscard]] const std::vector <file_entry>& files() const override;

            result_t <byte_vector> extract(const file_entry& entry) override;

        private:
            ha_archive();

            void_result_t parse();

            struct impl;
            std::unique_ptr <impl> m_pimpl;
    };
} // namespace crate
