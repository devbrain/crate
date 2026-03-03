cmake_minimum_required(VERSION 3.20)

project(crate
    VERSION 0.1.0
    DESCRIPTION "Crate library"
    LANGUAGES C CXX
)

# ============================================================================
# Neutrino CMake Integration
# ============================================================================

include(FetchContent)
include(GenerateExportHeader)

if(NOT DEFINED NEUTRINO_CMAKE_DIR)
    FetchContent_Declare(neutrino_cmake
        GIT_REPOSITORY https://github.com/devbrain/neutrino-cmake.git
        GIT_TAG master
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(neutrino_cmake)
    set(NEUTRINO_CMAKE_DIR "${neutrino_cmake_SOURCE_DIR}/cmake")
endif()

list(APPEND CMAKE_MODULE_PATH "${NEUTRINO_CMAKE_DIR}")
include(NeutrinoInit)

# =============================================================================
# C++ Standard
# =============================================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put DLLs and executables in the same directory so tests find shared libraries
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ============================================================================
# Options
# ============================================================================

neutrino_define_library_options(crate)
option(NEUTRINO_CRATE_WITH_LIBARCHIVE "Build with libarchive support" ON)
option(NEUTRINO_CRATE_WITH_ZSTD "Build with zstd compression support" ON)
option(NEUTRINO_CRATE_WITH_BROTLI "Build with brotli compression support" ON)
option(NEUTRINO_CRATE_WITH_BZIP2 "Build with bzip2 compression support" ON)
option(NEUTRINO_CRATE_WITH_XZ "Build with xz/lzma compression support" ON)

if(NEUTRINO_CRATE_BUILD_TESTS)
    set(NEUTRINO_CRATE_BUILD_SHARED ON CACHE BOOL "Build shared library for tests" FORCE)
endif()

# ============================================================================
# Dependencies
# ============================================================================

# Fetch tl::expected (std::expected backport for C++20)
include(${NEUTRINO_CMAKE_DIR}/deps/expected.cmake)
neutrino_fetch_expected()

# Fetch miniz sources (not as a library, we embed it directly)
# SOURCE_SUBDIR points to non-existent directory to prevent add_subdirectory
FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.1.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR __no_cmake__
)
FetchContent_MakeAvailable(miniz)

# Fetch and build libarchive (optional)
if(NEUTRINO_CRATE_WITH_LIBARCHIVE)
    # Configure libarchive to match crate's build mode
    if(NEUTRINO_CRATE_BUILD_SHARED)
        set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    else()
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    endif()

    # Disable all optional dependencies - core only
    set(ENABLE_ZLIB OFF CACHE BOOL "" FORCE)
    set(ENABLE_BZip2 OFF CACHE BOOL "" FORCE)
    set(ENABLE_LZMA OFF CACHE BOOL "" FORCE)
    set(ENABLE_ZSTD OFF CACHE BOOL "" FORCE)
    set(ENABLE_LZ4 OFF CACHE BOOL "" FORCE)
    set(ENABLE_LZO OFF CACHE BOOL "" FORCE)
    set(ENABLE_LIBB2 OFF CACHE BOOL "" FORCE)
    set(ENABLE_LIBXML2 OFF CACHE BOOL "" FORCE)
    set(ENABLE_EXPAT OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_LIBMD OFF CACHE BOOL "" FORCE)
    set(ENABLE_PCREPOSIX OFF CACHE BOOL "" FORCE)
    set(ENABLE_PCRE2POSIX OFF CACHE BOOL "" FORCE)
    set(ENABLE_LibGCC OFF CACHE BOOL "" FORCE)
    set(ENABLE_CNG OFF CACHE BOOL "" FORCE)
    set(ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
    set(ENABLE_NETTLE OFF CACHE BOOL "" FORCE)
    set(ENABLE_ICONV OFF CACHE BOOL "" FORCE)
    set(ENABLE_ACL OFF CACHE BOOL "" FORCE)
    set(ENABLE_XATTR OFF CACHE BOOL "" FORCE)

    # Disable tools and tests
    set(ENABLE_TAR OFF CACHE BOOL "" FORCE)
    set(ENABLE_CPIO OFF CACHE BOOL "" FORCE)
    set(ENABLE_CAT OFF CACHE BOOL "" FORCE)
    set(ENABLE_UNZIP OFF CACHE BOOL "" FORCE)
    set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
    set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(libarchive
        GIT_REPOSITORY https://github.com/libarchive/libarchive.git
        GIT_TAG v3.7.7
        GIT_SHALLOW TRUE
    )
    # libarchive's CMakeLists.txt uses cmake_minimum_required < 3.5,
    # which newer CMake rejects without this policy override.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    FetchContent_MakeAvailable(libarchive)
    unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)
endif()

# Fetch and build zstd (optional)
if(NEUTRINO_CRATE_WITH_ZSTD)
    # Configure zstd build options
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
    set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)

    # Match crate's build mode
    if(NEUTRINO_CRATE_BUILD_SHARED)
        set(ZSTD_BUILD_SHARED ON CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_STATIC OFF CACHE BOOL "" FORCE)
    else()
        set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    endif()

    FetchContent_Declare(zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.6
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR build/cmake
    )
    FetchContent_MakeAvailable(zstd)
endif()

# Fetch and build brotli (optional)
if(NEUTRINO_CRATE_WITH_BROTLI)
    # Configure brotli build options
    set(BROTLI_DISABLE_TESTS ON CACHE BOOL "" FORCE)
    set(BROTLI_BUNDLED_MODE ON CACHE BOOL "" FORCE)

    FetchContent_Declare(brotli
        GIT_REPOSITORY https://github.com/google/brotli.git
        GIT_TAG v1.1.0
        GIT_SHALLOW TRUE
        PATCH_COMMAND patch -p1 < ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/brotli-disable-tools.patch
    )
    FetchContent_MakeAvailable(brotli)
endif()

# Fetch bzip2 sources (optional) - embedded directly since bzip2 lacks CMake support
# SOURCE_SUBDIR points to non-existent directory to prevent add_subdirectory
if(NEUTRINO_CRATE_WITH_BZIP2)
    FetchContent_Declare(bzip2
        GIT_REPOSITORY https://gitlab.com/bzip2/bzip2.git
        GIT_TAG bzip2-1.0.8
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR __no_cmake__
    )
    FetchContent_MakeAvailable(bzip2)
endif()

# Fetch and build xz-utils/liblzma (optional)
if(NEUTRINO_CRATE_WITH_XZ)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CREATE_XZ_SYMLINKS OFF CACHE BOOL "" FORCE)
    set(CREATE_LZMA_SYMLINKS OFF CACHE BOOL "" FORCE)
    set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(xz
        GIT_REPOSITORY https://github.com/tukaani-project/xz.git
        GIT_TAG v5.6.3
        GIT_SHALLOW TRUE
        PATCH_COMMAND patch -p1 < ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/xz-disable-tools.patch
    )
    FetchContent_MakeAvailable(xz)
endif()

# ============================================================================
# Library Target
# ============================================================================

add_subdirectory(src/crate)

# ============================================================================
# Tests
# ============================================================================

if(NEUTRINO_CRATE_BUILD_TESTS)
    include(${NEUTRINO_CMAKE_DIR}/deps/doctest.cmake)
    neutrino_fetch_doctest()

    enable_testing()
    add_subdirectory(test)
endif()

# ============================================================================
# Examples
# ============================================================================

if(NEUTRINO_CRATE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ============================================================================
# Installation
# ============================================================================

# Note: When optional deps are enabled via FetchContent (not installed), we can't
# export the crate target properly. Skip install in this case.
if(NEUTRINO_CRATE_INSTALL AND NOT NEUTRINO_CRATE_WITH_LIBARCHIVE AND NOT NEUTRINO_CRATE_WITH_ZSTD AND NOT NEUTRINO_CRATE_WITH_BROTLI AND NOT NEUTRINO_CRATE_WITH_BZIP2 AND NOT NEUTRINO_CRATE_WITH_XZ)
    include(NeutrinoInstall)

    neutrino_install_headers(crate)

    # Install generated export header
    install(FILES
        ${PROJECT_BINARY_DIR}/include/crate/crate_export.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/crate
    )

    neutrino_install_library(crate
        NAMESPACE neutrino::
        COMPATIBILITY SameMajorVersion
    )
endif()

# ============================================================================
# Summary
# ============================================================================

neutrino_is_top_level(_is_top_level)
if(_is_top_level)
    neutrino_print_options(crate)
    message(STATUS "  With libarchive: ${NEUTRINO_CRATE_WITH_LIBARCHIVE}")
    message(STATUS "  With zstd:       ${NEUTRINO_CRATE_WITH_ZSTD}")
    message(STATUS "  With brotli:     ${NEUTRINO_CRATE_WITH_BROTLI}")
    message(STATUS "  With bzip2:      ${NEUTRINO_CRATE_WITH_BZIP2}")
    message(STATUS "  With xz:         ${NEUTRINO_CRATE_WITH_XZ}")
endif()
