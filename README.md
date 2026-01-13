# crate

A neutrino ecosystem compiled library

## Requirements

- CMake 3.20+
- C++20 compiler

## Building

```bash
cmake -B build
cmake --build build
```

## Testing

```bash
cmake -B build -DNEUTRINO_CRATE_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

## Installation

```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build
cmake --install build
```

## Usage

### FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(crate
    GIT_REPOSITORY https://github.com/devbrain/crate.git
    GIT_TAG main
)
FetchContent_MakeAvailable(crate)

target_link_libraries(your_target PRIVATE neutrino::crate)
```

### find_package

```cmake
find_package(crate REQUIRED CONFIG)
target_link_libraries(your_target PRIVATE neutrino::crate)
```

## License

MIT License - see LICENSE file for details.
