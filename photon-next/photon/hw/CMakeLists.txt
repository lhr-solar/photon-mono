add_library(photon_hw STATIC
    src/pcan.cpp
)
target_include_directories(photon_hw PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(photon_hw PUBLIC photon_wire photon_base)
photon_apply_warnings(photon_hw)
