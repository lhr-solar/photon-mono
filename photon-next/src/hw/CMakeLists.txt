add_library(photon_hw STATIC
    pcan.cpp
)
target_include_directories(photon_hw PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(photon_hw PUBLIC photon_wire photon_base)
photon_apply_warnings(photon_hw)
