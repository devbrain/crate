#include <../../../include/crate/core/vfs.hh>

namespace crate::vfs {
    file_reader::~file_reader() = default;

    result_t <size_t> file_reader::read(std::vector <u8>& buffer, size_t max_size) {
        size_t old_size = buffer.size();
        buffer.resize(old_size + max_size);
        auto result = read(buffer.data() + old_size, max_size);
        if (!result) {
            buffer.resize(old_size);
            return result;
        }
        buffer.resize(old_size + *result);
        return result;
    }

    u64 file_reader::remaining() const {
        return size() > position() ? size() - position() : 0;
    }

    memory_file_reader::memory_file_reader(byte_vector data)
        : data_(std::move(data)), pos_(0) {
    }

    result_t <size_t> memory_file_reader::read(u8* buffer, size_t size) {
        if (pos_ >= data_.size()) {
            return 0; // EOF
        }
        size_t available = data_.size() - pos_;
        size_t to_read = std::min(size, available);
        std::memcpy(buffer, data_.data() + pos_, to_read);
        pos_ += to_read;
        return to_read;
    }

    bool memory_file_reader::eof() const {
        return pos_ >= data_.size();
    }

    u64 memory_file_reader::position() const {
        return pos_;
    }

    u64 memory_file_reader::size() const {
        return data_.size();
    }

    bool archive_vfs::exists(std::string_view path) const {
        return find_node(path) != nullptr;
    }

    bool archive_vfs::is_file(std::string_view path) const {
        auto node = find_node(path);
        return node && node->type == entry_type::File;
    }

    bool archive_vfs::is_directory(std::string_view path) const {
        auto node = find_node(path);
        return node && node->type == entry_type::Directory;
    }

    std::optional <stat_info> archive_vfs::stat(std::string_view path) const {
        auto node = find_node(path);
        if (!node) return std::nullopt;

        stat_info st;
        st.type = node->type;

        if (node->entry) {
            st.size = node->entry->uncompressed_size;
            st.compressed_size = node->entry->compressed_size;
            st.mtime = node->entry->datetime;
            st.attribs = node->entry->attribs;
        }

        return st;
    }

    result_t <std::vector <dir_entry>> archive_vfs::readdir(std::string_view path) const {
        auto node = find_node(path);
        if (!node) {
            return std::unexpected(error(error_code::FileNotInArchive,
                                         "Directory not found: " + std::string(path)));
        }
        if (node->type != entry_type::Directory) {
            return std::unexpected(error(error_code::InvalidHeader,
                                         "Not a directory: " + std::string(path)));
        }

        std::vector <dir_entry> entries;
        entries.reserve(node->children.size());

        for (const auto& [name, child] : node->children) {
            dir_entry de;
            de.name = name;
            de.type = child->type;
            if (child->entry) {
                de.size = child->entry->uncompressed_size;
            }
            entries.push_back(std::move(de));
        }

        // Sort entries: directories first, then alphabetically
        std::sort(entries.begin(), entries.end(), [](const dir_entry& a, const dir_entry& b) {
            if (a.type != b.type) {
                return a.type == entry_type::Directory;
            }
            return a.name < b.name;
        });

        return entries;
    }

    result_t <std::unique_ptr <file_reader>> archive_vfs::open(std::string_view path) const {
        auto node = find_node(path);
        if (!node) {
            return std::unexpected(error(error_code::FileNotInArchive,
                                         "File not found: " + std::string(path)));
        }
        if (node->type != entry_type::File || !node->entry) {
            return std::unexpected(error(error_code::InvalidHeader,
                                         "Not a file: " + std::string(path)));
        }

        // Extract the file and wrap in MemoryFileReader
        auto data = archive_->extract(*node->entry);
        if (!data) {
            return std::unexpected(data.error());
        }

        return std::make_unique <memory_file_reader>(std::move(*data));
    }

    result_t <byte_vector> archive_vfs::read(std::string_view path) const {
        auto node = find_node(path);
        if (!node) {
            return std::unexpected(error(error_code::FileNotInArchive,
                                         "File not found: " + std::string(path)));
        }
        if (node->type != entry_type::File || !node->entry) {
            return std::unexpected(error(error_code::InvalidHeader,
                                         "Not a file: " + std::string(path)));
        }

        return archive_->extract(*node->entry);
    }

    result_t <std::vector <dir_entry>> archive_vfs::root() const {
        return readdir("");
    }

    size_t archive_vfs::file_count() const {
        return archive_->files().size();
    }

    archive* archive_vfs::get_archive() const {
        return archive_.get();
    }

    archive_vfs::archive_vfs() = default;

    void archive_vfs::build_tree() {
        root_ = std::make_unique <vfs_node>();
        root_->name = "";
        root_->type = entry_type::Directory;

        for (const auto& file : archive_->files()) {
            insert_path(file);
        }
    }

    void archive_vfs::insert_path(const file_entry& entry) const {
        std::string path = normalize_path(entry.name);
        if (path.empty()) return;

        vfs_node* current = root_.get();
        size_t start = 0;

        while (start < path.size()) {
            size_t end = path.find('/', start);
            if (end == std::string::npos) {
                end = path.size();
            }

            std::string component = path.substr(start, end - start);
            if (component.empty()) {
                start = end + 1;
                continue;
            }

            auto it = current->children.find(component);
            if (it == current->children.end()) {
                auto node = std::make_unique <vfs_node>();
                node->name = component;

                // If this is the last component, it's a file
                if (end == path.size()) {
                    node->type = entry_type::File;
                    node->entry = &entry;

                    // Check for directory by trailing slash or zero size
                    // (Many formats indicate directories this way)
                    if (!entry.name.empty() &&
                        (entry.name.back() == '/' || entry.name.back() == '\\')) {
                        node->type = entry_type::Directory;
                    }
                } else {
                    node->type = entry_type::Directory;
                }

                // Insert into current node's children, then advance current
                auto [inserted_it, success] = current->children.emplace(component, std::move(node));
                current = inserted_it->second.get();
            } else {
                current = it->second.get();
            }

            start = end + 1;
        }
    }

    std::string archive_vfs::normalize_path(std::string_view path) {
        std::string result;
        result.reserve(path.size());

        bool last_was_slash = true; // Skip leading slashes

        for (char c : path) {
            if (c == '\\' || c == '/') {
                if (!last_was_slash) {
                    result += '/';
                    last_was_slash = true;
                }
            } else {
                result += c;
                last_was_slash = false;
            }
        }

        // Remove trailing slash
        if (!result.empty() && result.back() == '/') {
            result.pop_back();
        }

        return result;
    }

    const vfs_node* archive_vfs::find_node(std::string_view path) const {
        std::string normalized = normalize_path(path);

        if (normalized.empty()) {
            return root_.get();
        }

        const vfs_node* current = root_.get();
        size_t start = 0;

        while (start < normalized.size()) {
            size_t end = normalized.find('/', start);
            if (end == std::string::npos) {
                end = normalized.size();
            }

            std::string component = normalized.substr(start, end - start);
            if (component.empty()) {
                start = end + 1;
                continue;
            }

            auto it = current->children.find(component);
            if (it == current->children.end()) {
                return nullptr;
            }

            current = it->second.get();
            start = end + 1;
        }

        return current;
    }
} // namespace crate::vfs
