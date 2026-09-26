add_library(photon_synth STATIC
    src/tracksim.cpp
)
target_include_directories(photon_synth PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(photon_synth PUBLIC photon_base)
photon_apply_warnings(photon_synth)
