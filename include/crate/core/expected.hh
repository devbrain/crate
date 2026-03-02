#pragma once

#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wsign-conversion"
#elif defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wsign-conversion"
#endif

#include <tl/expected.hpp>

#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#elif defined(__clang__)
#  pragma clang diagnostic pop
#endif

#include <type_traits>

namespace crate {
    template<typename T, typename E>
    using expected = tl::expected<T, E>;

    template<typename E>
    using unexpected = tl::unexpected<E>;

    template<typename E>
    constexpr auto make_unexpected(E&& e) {
        return tl::unexpected<std::decay_t<E>>(std::forward<E>(e));
    }
} // namespace crate
