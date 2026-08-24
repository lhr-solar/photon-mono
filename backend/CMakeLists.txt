add_executable(photon-backend
    src/cli.cpp
    src/main.cpp
    src/net.cpp
    src/session.cpp
    src/source.cpp
    src/track_catalog.cpp
    src/video_relay.cpp
    src/websocket.cpp
)

target_include_directories(photon-backend
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(photon-backend PRIVATE photon-core)

if(WIN32)
    target_link_libraries(photon-backend PRIVATE ws2_32)
endif()

file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets" PHOTON_ASSETS_DIR_NORM)
target_compile_definitions(photon-backend PRIVATE PHOTON_ASSETS_DIR="${PHOTON_ASSETS_DIR_NORM}")

add_custom_command(TARGET photon-backend POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:photon-backend>/assets"
)

photon_apply_warnings(photon-backend)
