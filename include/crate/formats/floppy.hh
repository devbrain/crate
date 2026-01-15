#pragma once

// Floppy disk image support (FAT12/FAT16)
// Read-only extraction interface for raw floppy images

#include <crate/formats/archive.hh>
#include <memory>

namespace crate {

class CRATE_EXPORT floppy_image : public archive {
public:
    using archive::extract;

    ~floppy_image() override;

    /// Open a floppy disk image
    /// @param data Raw disk image bytes (must remain valid for archive lifetime)
    /// @return Archive instance or error
    static result_t<std::unique_ptr<floppy_image>> open(byte_span data);

    /// Open a floppy disk image from file
    /// @param path Path to the image file
    /// @return Archive instance or error
    static result_t<std::unique_ptr<floppy_image>> open(const std::filesystem::path& path);

    /// Get list of all files
    /// Directories are not included; file paths include subdirectory components
    [[nodiscard]] const std::vector<file_entry>& files() const override;

    /// Extract a file to memory
    [[nodiscard]] result_t<byte_vector> extract(const file_entry& entry) override;

private:
    floppy_image();

    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace crate
