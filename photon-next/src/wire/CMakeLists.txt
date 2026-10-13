add_library(photon_wire STATIC
    protocol.cpp
    control.cpp
    canp.cpp
    video_index.cpp
)
target_include_directories(photon_wire PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(photon_wire PUBLIC photon_base)
photon_apply_warnings(photon_wire)
