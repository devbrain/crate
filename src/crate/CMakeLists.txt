# ============================================================================
# Source files
# ============================================================================

set(CRATE_SOURCES
        crate.cc
        compression/ace_lz.cc
        compression/arc_lzw.cc
        compression/lha_new.cc
        compression/arj_lz.cc
        compression/decompressor.cc
        compression/decompressing_stream.cc
        compression/streams.cc
        compression/diet.cc
        compression/explode.cc
        compression/ha.cc
        compression/hyp.cc
        compression/inflate.cc
        compression/kwaj.cc
        compression/lzh.cc
        compression/lzss.cc
        compression/lzx.cc
        compression/mszip.cc
        compression/quantum.cc
        compression/rar_15.cc
        compression/rar_29.cc
        compression/rar5.cc
        compression/rnc.cc
        compression/stuffit_rle.cc
        compression/stuffit_huffman.cc
        compression/stuffit_lzw.cc
        compression/stuffit_method13.cc
        compression/stuffit_arsenic.cc
        compression/rar_filters.cc
        compression/rar_ppm.cc
        compression/rar_unpack.cc
        compression/szdd.cc
        compression/zoo_lzw.cc
        core/bitstream.cc
        core/crc.cc
        core/error.cc
        core/huffman.cc
        core/lru_cache.cc
        core/path.cc
        core/system.cc
        core/types.cc
        crypto/aes_decoder.cc
        crypto/rar_crypt.cc
        crypto/sha.cc
        formats/ace.cc
        formats/arc.cc
        formats/archive.cc
        formats/arj.cc
        formats/cab.cc
        formats/chm.cc
        formats/ha.cc
        formats/hyp.cc
        formats/lha.cc
        formats/rar.cc
        formats/stuffit.cc
        formats/zip.cc
        formats/zoo.cc
        fs/fat_bpb.cc
        fs/fat_table.cc
        fs/fat_directory.cc
        fs/floppy.cc
)

set(CRATE_PUBLIC_HEADERS
        ${PROJECT_SOURCE_DIR}/include/crate/crate.hh
        ${PROJECT_SOURCE_DIR}/include/crate/compression/diet.hh
        ${PROJECT_SOURCE_DIR}/include/crate/compression/explode.hh
        ${PROJECT_SOURCE_DIR}/include/crate/compression/inflate.hh
        ${PROJECT_SOURCE_DIR}/include/crate/compression/kwaj.hh
        ${PROJECT_SOURCE_DIR}/include/crate/compression/szdd.hh
        ${PROJECT_SOURCE_DIR}/include/crate/core/decompressor.hh
        ${PROJECT_SOURCE_DIR}/include/crate/compression/streams.hh
        ${PROJECT_SOURCE_DIR}/include/crate/core/decompressing_stream.hh
        ${PROJECT_SOURCE_DIR}/include/crate/core/error.hh
        ${PROJECT_SOURCE_DIR}/include/crate/core/system.hh
        ${PROJECT_SOURCE_DIR}/include/crate/core/types.hh
        ${PROJECT_SOURCE_DIR}/include/crate/formats/ace.hh
        ${PROJECT_SOURCE_DIR}/include/crate/formats/arc.hh
        ${PROJECT_SOURCE_DIR}/include/crate/formats/archive.hh
        ${PROJECT_SOURCE_DIR}/include/crate/formats/arj.hh
        ${PROJECT_SOURCE_DIR}/include/crate/formats/cab.hh
        ${PROJECT_SOURCE_DIR}/include/crate/formats/chm.hh
        ${PROJECT_SOURCE_DIR}/include/crate/formats/ha.hh
        ${PROJECT_SOURCE_DIR}/include/crate/formats/hyp.hh
        ${PROJECT_SOURCE_DIR}/include/crate/formats/lha.hh
        ${PROJECT_SOURCE_DIR}/include/crate/formats/rar.hh
        ${PROJECT_SOURCE_DIR}/include/crate/formats/stuffit.hh
        ${PROJECT_SOURCE_DIR}/include/crate/formats/zip.hh
        ${PROJECT_SOURCE_DIR}/include/crate/formats/zoo.hh
        ${PROJECT_SOURCE_DIR}/include/crate/formats/floppy.hh
)

set(CRATE_PRIVATE_HEADERS
        include/crate/compression/ace_lz.hh
        include/crate/compression/arc_lzw.hh
        include/crate/compression/arj_lz.hh
        include/crate/compression/ha.hh
        include/crate/compression/hyp.hh
        include/crate/compression/lha_new.hh
        include/crate/compression/lzh.hh
        include/crate/compression/lzss.hh
        include/crate/compression/lzx.hh
        include/crate/compression/mszip.hh
        include/crate/compression/quantum.hh
        include/crate/compression/rar_15.hh
        include/crate/compression/rar_29.hh
        include/crate/compression/rar5.hh
        include/crate/compression/rar_common.hh
        include/crate/compression/rar_filters.hh
        include/crate/compression/rar_ppm.hh
        include/crate/compression/rar_unpack.hh
        include/crate/compression/rnc.hh
        include/crate/compression/stuffit_rle.hh
        include/crate/compression/stuffit_huffman.hh
        include/crate/compression/stuffit_lzw.hh
        include/crate/compression/stuffit_method13.hh
        include/crate/compression/stuffit_arsenic.hh
        include/crate/compression/zoo_lzw.hh
        include/crate/core/bitstream.hh
        include/crate/core/crc.hh
        include/crate/core/huffman.hh
        include/crate/core/lru_cache.hh
        include/crate/core/path.hh
        include/crate/crypto/aes_decoder.hh
        include/crate/crypto/rar_crypt.hh
        include/crate/crypto/sha.hh
        include/crate/formats/arc_internal.hh
        include/crate/formats/hyp_internal.hh
        include/crate/fs/fat_bpb.hh
        include/crate/fs/fat_table.hh
        include/crate/fs/fat_directory.hh
)

# Optional libarchive wrapper sources
if(NEUTRINO_CRATE_WITH_LIBARCHIVE)
    list(APPEND CRATE_SOURCES
        formats/libarchive_archive.cc
    )
    list(APPEND CRATE_PUBLIC_HEADERS
        ${PROJECT_SOURCE_DIR}/include/crate/formats/libarchive_archive.hh
    )
endif()

# Optional zstd decompressor sources
if(NEUTRINO_CRATE_WITH_ZSTD)
    list(APPEND CRATE_SOURCES
        compression/zstd.cc
    )
    list(APPEND CRATE_PUBLIC_HEADERS
        ${PROJECT_SOURCE_DIR}/include/crate/compression/zstd.hh
    )
endif()

# Optional brotli decompressor sources
if(NEUTRINO_CRATE_WITH_BROTLI)
    list(APPEND CRATE_SOURCES
        compression/brotli.cc
    )
    list(APPEND CRATE_PUBLIC_HEADERS
        ${PROJECT_SOURCE_DIR}/include/crate/compression/brotli.hh
    )
endif()

# Optional bzip2 decompressor sources
if(NEUTRINO_CRATE_WITH_BZIP2)
    list(APPEND CRATE_SOURCES
        compression/bzip2.cc
    )
    list(APPEND CRATE_PUBLIC_HEADERS
        ${PROJECT_SOURCE_DIR}/include/crate/compression/bzip2.hh
    )
endif()

# Optional xz/lzma decompressor sources
if(NEUTRINO_CRATE_WITH_XZ)
    list(APPEND CRATE_SOURCES
        compression/xz.cc
    )
    list(APPEND CRATE_PUBLIC_HEADERS
        ${PROJECT_SOURCE_DIR}/include/crate/compression/xz.hh
    )
endif()

# Embedded third-party sources (miniz 3.x has split source files)
set(CRATE_THIRDPARTY_SOURCES
        ${miniz_SOURCE_DIR}/miniz.c
        ${miniz_SOURCE_DIR}/miniz_tinfl.c
        ${miniz_SOURCE_DIR}/miniz_tdef.c
)

# Embed bzip2 sources (no CMake build system available)
if(NEUTRINO_CRATE_WITH_BZIP2)
    list(APPEND CRATE_THIRDPARTY_SOURCES
        ${bzip2_SOURCE_DIR}/blocksort.c
        ${bzip2_SOURCE_DIR}/huffman.c
        ${bzip2_SOURCE_DIR}/crctable.c
        ${bzip2_SOURCE_DIR}/randtable.c
        ${bzip2_SOURCE_DIR}/compress.c
        ${bzip2_SOURCE_DIR}/decompress.c
        ${bzip2_SOURCE_DIR}/bzlib.c
    )
endif()

# ============================================================================
# Library target
# ============================================================================

neutrino_library_type(crate _lib_type)

add_library(crate ${_lib_type}
        ${CRATE_SOURCES}
        ${CRATE_PUBLIC_HEADERS}
        ${CRATE_PRIVATE_HEADERS}
        ${CRATE_THIRDPARTY_SOURCES}
)
add_library(neutrino::crate ALIAS crate)

target_compile_features(crate PUBLIC cxx_std_20)

# Generate export header
generate_export_header(crate
        EXPORT_FILE_NAME ${PROJECT_BINARY_DIR}/include/crate/crate_export.h
)

# Mark tl::expected includes as SYSTEM to suppress warnings from third-party code
get_target_property(_expected_inc tl::expected INTERFACE_INCLUDE_DIRECTORIES)
if(_expected_inc)
    target_include_directories(crate SYSTEM PUBLIC ${_expected_inc})
endif()
target_link_libraries(crate PUBLIC tl::expected)

target_include_directories(crate
        PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${miniz_SOURCE_DIR}>
)

# Miniz configuration - must match the defines in inflate.cc
# Also disable warnings for third-party code
set_source_files_properties(
    ${miniz_SOURCE_DIR}/miniz.c
    ${miniz_SOURCE_DIR}/miniz_tinfl.c
    ${miniz_SOURCE_DIR}/miniz_tdef.c
    PROPERTIES
        COMPILE_DEFINITIONS "MINIZ_NO_STDIO;MINIZ_NO_TIME;MINIZ_NO_ARCHIVE_APIS;MINIZ_NO_ARCHIVE_WRITING_APIS;MINIZ_NO_ZLIB_COMPATIBLE_NAMES"
        COMPILE_OPTIONS "-w"
)

# ============================================================================
# Optional libarchive support
# ============================================================================

if(NEUTRINO_CRATE_WITH_LIBARCHIVE)
    target_compile_definitions(crate PUBLIC CRATE_WITH_LIBARCHIVE)

    target_include_directories(crate PRIVATE
        $<BUILD_INTERFACE:${libarchive_SOURCE_DIR}/libarchive>
        $<BUILD_INTERFACE:${libarchive_BINARY_DIR}>
    )

    target_link_libraries(crate PRIVATE archive)
endif()

# ============================================================================
# Optional zstd support
# ============================================================================

if(NEUTRINO_CRATE_WITH_ZSTD)
    target_compile_definitions(crate PUBLIC CRATE_WITH_ZSTD)

    target_include_directories(crate PRIVATE
        $<BUILD_INTERFACE:${zstd_SOURCE_DIR}/lib>
    )

    # Link to appropriate zstd target based on build mode
    if(NEUTRINO_CRATE_BUILD_SHARED)
        target_link_libraries(crate PRIVATE libzstd_shared)
    else()
        target_link_libraries(crate PRIVATE libzstd_static)
    endif()
endif()

# ============================================================================
# Optional brotli support
# ============================================================================

if(NEUTRINO_CRATE_WITH_BROTLI)
    target_compile_definitions(crate PUBLIC CRATE_WITH_BROTLI)

    target_include_directories(crate PRIVATE
        $<BUILD_INTERFACE:${brotli_SOURCE_DIR}/c/include>
    )

    # Brotli provides static libraries: brotlidec, brotlienc, brotlicommon
    # We only need the decoder
    target_link_libraries(crate PRIVATE brotlidec brotlicommon)
endif()

# ============================================================================
# Optional bzip2 support
# ============================================================================

if(NEUTRINO_CRATE_WITH_BZIP2)
    target_compile_definitions(crate PUBLIC CRATE_WITH_BZIP2)

    target_include_directories(crate PRIVATE
        $<BUILD_INTERFACE:${bzip2_SOURCE_DIR}>
    )

    # Disable warnings for bzip2 third-party sources
    set_source_files_properties(
        ${bzip2_SOURCE_DIR}/blocksort.c
        ${bzip2_SOURCE_DIR}/huffman.c
        ${bzip2_SOURCE_DIR}/crctable.c
        ${bzip2_SOURCE_DIR}/randtable.c
        ${bzip2_SOURCE_DIR}/compress.c
        ${bzip2_SOURCE_DIR}/decompress.c
        ${bzip2_SOURCE_DIR}/bzlib.c
        PROPERTIES
            COMPILE_OPTIONS "-w"
    )
endif()

# ============================================================================
# Optional xz/lzma support
# ============================================================================

if(NEUTRINO_CRATE_WITH_XZ)
    target_compile_definitions(crate PUBLIC CRATE_WITH_XZ)

    target_include_directories(crate PRIVATE
        $<BUILD_INTERFACE:${xz_SOURCE_DIR}/src/liblzma/api>
    )

    target_link_libraries(crate PRIVATE liblzma)
endif()

# ============================================================================
# Compiler Warnings
# ============================================================================

neutrino_target_warnings(crate)
neutrino_target_sanitizers(crate)
