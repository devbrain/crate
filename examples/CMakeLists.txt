add_executable(crate_example
    example.cc
)

target_link_libraries(crate_example PRIVATE
    crate
)

neutrino_target_warnings(crate_example)
