#pragma once

#include <crate/formats/archive.hh>

namespace crate {

class CRATE_EXPORT zip_archive : public archive {
public:
    using archive::extract;

    ~zip_archive() override;

    static result_t<std::unique_ptr<zip_archive>> open(byte_span data);
    static result_t<std::unique_ptr<zip_archive>> open(const std::filesystem::path& path);
    static result_t<std::unique_ptr<zip_archive>> open(std::istream& stream);

    const std::vector<file_entry>& files() const override;
    result_t<byte_vector> extract(const file_entry& entry) override;

private:
    zip_archive();
    void_result_t parse();

    struct impl;
    std::unique_ptr<impl> pimpl_;
};

} // namespace crate
