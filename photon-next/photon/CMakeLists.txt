add_subdirectory(base)
add_subdirectory(synth)
if(NOT EMSCRIPTEN)
    add_subdirectory(array_sim)
endif()
add_subdirectory(wire)
add_subdirectory(decode)
add_subdirectory(store)
add_subdirectory(net)
if(NOT EMSCRIPTEN)
    add_subdirectory(hw)
endif()

add_library(photon-core INTERFACE)
target_link_libraries(photon-core INTERFACE
    photon_base
    photon_synth
    photon_wire
    photon_decode
    photon_store
    photon_net
)
if(NOT EMSCRIPTEN)
    target_link_libraries(photon-core INTERFACE photon_hw)
endif()
