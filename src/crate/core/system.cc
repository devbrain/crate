#include <crate/core/system.hh>
#include <array>

namespace crate {

result_t<byte_vector> read_stream(std::istream& is) {
    byte_vector buf;

    // If the stream is seekable, pre-allocate
    auto start = is.tellg();
    if (start >= 0) {
        is.seekg(0, std::ios::end);
        auto end = is.tellg();
        if (end >= 0 && end > start) {
            buf.reserve(static_cast<size_t>(end - start));
        }
        is.seekg(start);
        is.clear(); // clear any eofbit/failbit from seekg
    }

    std::array<char, 64 * 1024> tmp{};
    while (is.read(tmp.data(), static_cast<std::streamsize>(tmp.size())) || is.gcount() > 0) {
        auto n = static_cast<size_t>(is.gcount());
        buf.insert(buf.end(),
                   reinterpret_cast<const byte*>(tmp.data()),
                   reinterpret_cast<const byte*>(tmp.data()) + n);
    }

    if (is.bad()) {
        return crate::make_unexpected(error{error_code::ReadError, "Failed to read from stream"});
    }

    return buf;
}

} // namespace crate
