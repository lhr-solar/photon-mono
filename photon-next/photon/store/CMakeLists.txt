add_library(photon_store STATIC
    src/tsdb.cpp
    src/tsdb_live.cpp
    src/vsd1.cpp
    src/video_index.cpp
    src/log_sessions.cpp
)
target_include_directories(photon_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(photon_store PUBLIC photon_decode photon_wire photon_base photon_net)
photon_apply_warnings(photon_store)
