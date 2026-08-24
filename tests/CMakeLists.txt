add_executable(photon-tests
    test_main.cpp
    test_cli.cpp
    test_dbc.cpp
    test_pcan.cpp
    test_vsd1.cpp
    test_tracksim.cpp
    test_video.cpp
    ${CMAKE_SOURCE_DIR}/backend/src/cli.cpp
)

target_link_libraries(photon-tests PRIVATE photon-core)
target_include_directories(photon-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/backend/include
)

file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets" PHOTON_ASSETS_DIR_NORM)
target_compile_definitions(photon-tests PRIVATE PHOTON_ASSETS_DIR="${PHOTON_ASSETS_DIR_NORM}")

photon_apply_warnings(photon-tests)

add_test(NAME photon-protocol COMMAND photon-tests)
