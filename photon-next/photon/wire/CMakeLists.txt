add_library(photon_wire STATIC
    src/protocol.cpp
    src/control.cpp
    src/canp.cpp
)
target_include_directories(photon_wire PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(photon_wire PUBLIC photon_base photon_synth)
photon_apply_warnings(photon_wire)
