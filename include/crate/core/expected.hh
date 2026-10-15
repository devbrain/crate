#pragma once

#include <failsafe/expected.hh>

namespace crate {
    template<typename T, typename E>
    using expected = failsafe::expected<T, E>;

    template<typename E>
    using unexpected = failsafe::unexpected<E>;

    using failsafe::unexpect_t;
    using failsafe::unexpect;
    using failsafe::bad_expected_access;

    using failsafe::make_unexpected;
    using failsafe::make_unexpectedf;

    using failsafe::using_std_expected;
    using failsafe::expected_implementation;
} // namespace crate
