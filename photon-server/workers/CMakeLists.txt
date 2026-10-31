# These tools have no WebSocket/session server. Go owns their lifecycle.
add_executable(photon-pcan-adapter pcan_adapter.cpp)
target_link_libraries(photon-pcan-adapter PRIVATE photon_hw photon_net photon_wire)
photon_apply_warnings(photon-pcan-adapter)

add_executable(photon-tsdb-export tsdb_export.cpp)
target_link_libraries(photon-tsdb-export PRIVATE photon-export-lib photon_store photon_base photon_wire)
photon_apply_warnings(photon-tsdb-export)
foreach(target photon-pcan-adapter photon-tsdb-export)
    if(PHOTON_SHIP_DIR)
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PHOTON_SHIP_DIR}")
    endif()
endforeach()
