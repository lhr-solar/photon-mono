add_subdirectory(base)
add_subdirectory(wire)
add_subdirectory(decode EXCLUDE_FROM_ALL)
add_subdirectory(store EXCLUDE_FROM_ALL)
add_subdirectory(net)
if(NOT EMSCRIPTEN)
    add_subdirectory(hw EXCLUDE_FROM_ALL)
endif()

add_library(photon-core INTERFACE)
add_library(photon-client-core INTERFACE)
target_link_libraries(photon-client-core INTERFACE photon_base photon_wire photon_net)
target_link_libraries(photon-core INTERFACE
    photon_base
    photon_wire
    photon_decode
    photon_store
    photon_net
)
if(NOT EMSCRIPTEN)
    target_link_libraries(photon-core INTERFACE photon_hw)
endif()
