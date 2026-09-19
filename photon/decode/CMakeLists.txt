add_library(photon_decode STATIC
    src/dbc.cpp
)
target_include_directories(photon_decode PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(photon_decode PUBLIC photon_base)
photon_apply_warnings(photon_decode)
