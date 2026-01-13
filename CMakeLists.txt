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

# ============================================================================
# Options
# ============================================================================

neutrino_define_library_options(crate)
option(NEUTRINO_CRATE_WITH_LIBARCHIVE "Build with libarchive support" OFF)

if(NEUTRINO_CRATE_BUILD_TESTS)
    set(NEUTRINO_CRATE_BUILD_SHARED ON CACHE BOOL "Build shared library for tests" FORCE)
endif()

# ============================================================================
# Dependencies
# ============================================================================

# Fetch miniz sources (not as a library, we embed it directly)
FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.1.0
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(miniz)
if(NOT miniz_POPULATED)
    FetchContent_Populate(miniz)
endif()

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
    FetchContent_MakeAvailable(libarchive)
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

# Note: When libarchive is enabled via FetchContent (not installed), we can't
# export the crate target properly. Skip install in this case.
if(NEUTRINO_CRATE_INSTALL AND NOT NEUTRINO_CRATE_WITH_LIBARCHIVE)
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
endif()
