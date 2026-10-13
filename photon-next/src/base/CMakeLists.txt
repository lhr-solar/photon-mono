add_library(photon_base STATIC
    json.cpp
)
target_include_directories(photon_base PUBLIC ${CMAKE_SOURCE_DIR}/src)
photon_apply_warnings(photon_base)
