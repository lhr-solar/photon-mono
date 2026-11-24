# test_session_history.cpp and test_log_switch.cpp drive photon-backend over the v1 protocol.
# They are skipped until step 3 (cleanup removes the v1 backend); the files are kept.
add_executable(photon-tests
    test_main.cpp
    test_cli.cpp
    test_analysis_compute.cpp
    test_interp.cpp
    test_mux.cpp
    test_protocol_v2.cpp
    test_live_v2.cpp
    test_control_v2.cpp
    test_dbc.cpp
    test_log_sessions.cpp
    test_pcan.cpp
    test_vsd1.cpp
    test_tracksim.cpp
    test_tsdb.cpp
    test_tsdb_live.cpp
    test_tile_cache.cpp
    test_analysis_runtime.cpp
    test_analysis_workbook.cpp
    test_analysis_overlay.cpp
    test_plot_axis.cpp
    test_tile_persist.cpp
    test_replay_views.cpp
    test_live_fanout.cpp
    ${CMAKE_SOURCE_DIR}/tools/photon-index/live_fanout.cpp
    test_video.cpp
    test_video_index.cpp
    ${CMAKE_SOURCE_DIR}/photon/ui/src/tile_cache.cpp
    ${CMAKE_SOURCE_DIR}/photon/ui/src/analysis_runtime.cpp
    ${CMAKE_SOURCE_DIR}/photon/ui/src/analysis_workbook.cpp
    ${CMAKE_SOURCE_DIR}/photon/ui/src/analysis_eval.cpp
    ${CMAKE_SOURCE_DIR}/photon/ui/src/tile_store.cpp
    ${CMAKE_SOURCE_DIR}/photon/ui/src/live_v2.cpp
)

target_link_libraries(photon-tests PRIVATE photon-core photon-export-lib photon_daemon)
target_include_directories(photon-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/photon/daemon/include
    ${CMAKE_SOURCE_DIR}/photon/ui/include
    ${CMAKE_SOURCE_DIR}/tools/photon-index
)

if(WIN32)
    target_link_libraries(photon-tests PRIVATE ws2_32)
endif()

file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets" PHOTON_ASSETS_DIR_NORM)
target_compile_definitions(photon-tests PRIVATE PHOTON_ASSETS_DIR="${PHOTON_ASSETS_DIR_NORM}")

photon_apply_warnings(photon-tests)

add_test(NAME photon-protocol COMMAND photon-tests)

add_executable(photon-export-tests test_export.cpp)
target_link_libraries(photon-export-tests PRIVATE photon-export-lib)
target_include_directories(photon-export-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
photon_apply_warnings(photon-export-tests)
add_test(NAME photon-export COMMAND photon-export-tests)

# Protocol v2 probe: the real UI client against a running photon-server, for the
# end-to-end check. A console tool, so it is not registered with ctest.
add_executable(photon-v2-probe
    probe_v2.cpp
    ${CMAKE_SOURCE_DIR}/photon/ui/src/client.cpp
    ${CMAKE_SOURCE_DIR}/photon/ui/src/arena.cpp
    ${CMAKE_SOURCE_DIR}/photon/ui/src/live_v2.cpp
    ${CMAKE_SOURCE_DIR}/photon/ui/src/signal_map.cpp
    ${CMAKE_SOURCE_DIR}/photon/ui/src/alarms.cpp
    ${CMAKE_SOURCE_DIR}/photon/ui/src/theme.cpp
    ${CMAKE_SOURCE_DIR}/photon/ui/src/tile_cache.cpp
    ${CMAKE_SOURCE_DIR}/photon/ui/src/tile_store.cpp
    ${CMAKE_SOURCE_DIR}/photon/ui/src/analysis_runtime.cpp
    ${CMAKE_SOURCE_DIR}/photon/ui/src/page_url.cpp
    ${CMAKE_SOURCE_DIR}/photon/ui/src/platform.cpp
)
# photon_ui/arena.hpp pulls in imgui.h (alarms.hpp) and platform.cpp includes SDL.h, so the
# probe takes both from photon-imgui (headers, the imgui core, SDL2). Only the objects it
# references are linked; no UI app sources and no GL backends.
target_link_libraries(photon-v2-probe PRIVATE photon-core photon-imgui)
target_include_directories(photon-v2-probe PRIVATE ${CMAKE_SOURCE_DIR}/photon/ui/include)
if(WIN32)
    target_link_libraries(photon-v2-probe PRIVATE ws2_32)
endif()
photon_apply_warnings(photon-v2-probe)
