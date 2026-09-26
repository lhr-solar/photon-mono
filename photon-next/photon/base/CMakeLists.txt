add_library(photon_base STATIC
    src/json.cpp
)
target_include_directories(photon_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
photon_apply_warnings(photon_base)
