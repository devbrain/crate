# Centralized test data directory
set(TESTDATA_DIR "${CMAKE_SOURCE_DIR}/testdata")

# Configure test data paths
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/crate")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/test_config.hh.in"
    "${CMAKE_CURRENT_BINARY_DIR}/crate/test_config.hh"
)

add_executable(crate_tests
    test_ace.cc
    test_arc.cc
    test_arj.cc
    test_bitstream.cc
    test_cab.cc
    test_chm.cc
    test_crate.cc
    test_ha.cc
    test_huffman.cc
    test_hyp.cc
    test_kwaj.cc
    test_lha.cc
    test_lzss.cc
    test_lzx.cc
    test_explode.cc
    test_main.cc
    test_mszip.cc
    test_progress.cc
    test_rar.cc
    test_rar_corpus.cc
    test_stuffit.cc
    test_szdd.cc
    test_vfs.cc
    test_zoo.cc
    $<TARGET_OBJECTS:crate_objects>
)

target_link_libraries(crate_tests PRIVATE
    doctest::doctest
    $<$<BOOL:${NEUTRINO_CRATE_WITH_LIBARCHIVE}>:archive>
)

target_compile_features(crate_tests PRIVATE cxx_std_23)

target_include_directories(crate_tests PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_BINARY_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src/crate/include"
)

# Define CRATE_STATIC_DEFINE so export macros expand to nothing
target_compile_definitions(crate_tests PRIVATE CRATE_STATIC_DEFINE)

# Optional libarchive support
if(NEUTRINO_CRATE_WITH_LIBARCHIVE)
    target_compile_definitions(crate_tests PRIVATE CRATE_WITH_LIBARCHIVE)
    target_include_directories(crate_tests PRIVATE
        "${libarchive_SOURCE_DIR}/libarchive"
        "${libarchive_BINARY_DIR}"
    )
endif()

neutrino_target_warnings(crate_tests)
neutrino_target_sanitizers(crate_tests)

include(CTest)
add_test(NAME crate_tests COMMAND crate_tests)
