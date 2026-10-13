add_library(photon_decode STATIC
    dbc.cpp
    decode_buffer.cpp
)
target_include_directories(photon_decode PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(photon_decode PUBLIC photon_base photon_wire)
photon_apply_warnings(photon_decode)
