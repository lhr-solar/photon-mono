add_executable(photon-tests
    test_main.cpp
    test_interp.cpp
    test_mux.cpp
    test_dbc.cpp
    test_log_sessions.cpp
    test_pcan.cpp
    test_vsd1.cpp
    test_tsdb.cpp
    test_tsdb_live.cpp
    test_tile_cache.cpp
    test_analysis_runtime.cpp
    test_analysis_workbook.cpp
    test_analysis_overlay.cpp
    test_plot_axis.cpp
    test_chart_grid.cpp
    test_tile_persist.cpp
    test_live_fanout.cpp
    ${CMAKE_SOURCE_DIR}/tools/photon-index/live_fanout.cpp
    test_video.cpp
    test_video_index.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/tile_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/analysis_runtime.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/analysis_workbook.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/analysis_eval.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/tile_store.cpp
)

target_link_libraries(photon-tests PRIVATE photon-core photon-export-lib)
target_include_directories(photon-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tools/photon-index
    ${CMAKE_SOURCE_DIR}/../photon-server/workers
)

if(WIN32)
    target_link_libraries(photon-tests PRIVATE ws2_32)
endif()

file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets" PHOTON_ASSETS_DIR_NORM)
target_compile_definitions(photon-tests PRIVATE PHOTON_ASSETS_DIR="${PHOTON_ASSETS_DIR_NORM}")

photon_apply_warnings(photon-tests)

add_test(NAME photon-protocol COMMAND photon-tests)

if(PHOTON_NATIVE_UI)
    add_executable(photon-chart-interaction-tests
        test_chart_interactions.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/chart_grid.cpp
    )
    target_link_libraries(photon-chart-interaction-tests PRIVATE photon-core photon-imgui)
    target_include_directories(photon-chart-interaction-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME photon-chart-interactions COMMAND photon-chart-interaction-tests)
endif()

add_executable(photon-view-document-tests test_custom_views.cpp ${CMAKE_SOURCE_DIR}/src/ui/view_document.cpp)
target_link_libraries(photon-view-document-tests PRIVATE photon-core)
target_include_directories(photon-view-document-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME photon-view-documents COMMAND photon-view-document-tests)
