add_executable(photon-tests
    test_main.cpp
    test_cli.cpp
    test_mux.cpp
    test_dbc.cpp
    test_vsd1.cpp
    test_live_fanout.cpp
    ${CMAKE_SOURCE_DIR}/tools/photon-index/live_fanout.cpp
    test_video.cpp
    test_video_index.cpp
)

target_link_libraries(photon-tests PRIVATE photon-core photon_daemon)
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
