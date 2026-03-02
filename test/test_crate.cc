#include <doctest/doctest.h>
#include <crate/crate.hh>

TEST_CASE("crate version info") {
    CHECK(crate::VERSION_MAJOR >= 1);
    CHECK(crate::VERSION_STRING != nullptr);
    CHECK(std::string(crate::VERSION_STRING).size() > 0);
}
