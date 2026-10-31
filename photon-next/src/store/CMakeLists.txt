add_library(photon_store STATIC
    tsdb.cpp
    tsdb_live.cpp
    vsd1.cpp
    log_sessions.cpp
)
target_include_directories(photon_store PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(photon_store PUBLIC photon_decode photon_wire photon_base photon_net)
photon_apply_warnings(photon_store)
