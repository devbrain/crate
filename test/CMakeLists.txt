# Centralized test data directory
set(TESTDATA_DIR "${CMAKE_SOURCE_DIR}/testdata")

# Configure test data paths
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/crate")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/test_config.hh.in"
    "${CMAKE_CURRENT_BINARY_DIR}/crate/test_config.hh"
)

set(MD5_SOURCE md5.c)

add_executable(crate_tests
    ${MD5_SOURCE}
    test_ace.cc
    test_arc.cc
    test_arj.cc
    test_bitstream.cc
    test_cab.cc
    test_chm.cc
    test_crate.cc
    test_diet.cc
    test_ha.cc
    test_huffman.cc
    test_hyp.cc
    test_istream.cc
    test_kwaj.cc
    test_lha.cc
    test_libarchive.cc
    test_lzss.cc
    test_lzx.cc
    test_lzx_quantum_streaming.cc
    test_explode.cc
    test_main.cc
    test_mszip.cc
    test_progress.cc
    test_rar.cc
    test_rar_corpus.cc
    test_rar_streaming.cc
    test_rnc.cc
    test_stuffit.cc
    test_szdd.cc
    test_floppy.cc
    test_zip.cc
    test_zoo.cc
)

target_link_libraries(crate_tests PRIVATE
    crate
    doctest::doctest
)

target_compile_features(crate_tests PRIVATE cxx_std_20)

target_include_directories(crate_tests PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/src/crate/include"
)

# Define CRATE_STATIC_DEFINE so export macros expand to nothing
target_compile_definitions(crate_tests PRIVATE CRATE_STATIC_DEFINE)

# Disable warnings for third-party md5.c (public domain, Solar Designer)
set_source_files_properties(${MD5_SOURCE} PROPERTIES COMPILE_OPTIONS "-w")

neutrino_target_warnings(crate_tests)
neutrino_target_sanitizers(crate_tests)

include(CTest)
add_test(NAME crate_tests COMMAND crate_tests)
