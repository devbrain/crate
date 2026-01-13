# Neutrino CMake Integration Guide

This document explains how to use **neutrino-cmake** in this project.

## What is neutrino-cmake?

neutrino-cmake is a centralized CMake tooling repository for the Neutrino C++ ecosystem. It provides:

- **Standardized build options** - Consistent option naming across all projects
- **Compiler warnings** - Pre-configured strict warning flags for MSVC, GCC, and Clang
- **Sanitizers** - Easy integration of ASan, UBSan, TSan, MSan
- **Dependency management** - Ready-to-use fetch recipes for common dependencies
- **Installation helpers** - Consistent package configuration file generation

## Available Options

This project defines the following CMake options:

| Option | Default | Description |
|--------|---------|-------------|
| `NEUTRINO_CRATE_BUILD_TESTS` | ON (top-level) | Build unit tests |
| `NEUTRINO_CRATE_BUILD_EXAMPLES` | ON (top-level) | Build examples |
| `NEUTRINO_CRATE_BUILD_BENCHMARKS` | OFF | Build benchmarks |
| `NEUTRINO_CRATE_INSTALL` | ON (top-level) | Enable installation |

## Configuration Examples

### Development Build

```bash
cmake -B build \
    -DNEUTRINO_CRATE_BUILD_TESTS=ON \
    -DNEUTRINO_CRATE_BUILD_EXAMPLES=ON \
    -DNEUTRINO_ENABLE_ASAN=ON \
    -DNEUTRINO_ENABLE_UBSAN=ON

cmake --build build
ctest --test-dir build
```

### Release Build

```bash
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local

cmake --build build
cmake --install build
```

## Adding Dependencies

To add a neutrino ecosystem dependency:

```cmake
include(${NEUTRINO_CMAKE_DIR}/deps/failsafe.cmake)
neutrino_fetch_failsafe()

target_link_libraries(crate PUBLIC neutrino::failsafe)
```

Available dependencies: failsafe, euler, mio, libiff, scaler, mz-explode, datascript, sdlpp, SDL2, SDL3, imgui, doctest, benchmark, and more.

## Further Reading

- [neutrino-cmake repository](https://github.com/devbrain/neutrino-cmake)
- [CMake FetchContent documentation](https://cmake.org/cmake/help/latest/module/FetchContent.html)
