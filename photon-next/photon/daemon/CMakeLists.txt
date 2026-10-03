add_library(photon_daemon STATIC
    src/analysis_compute.cpp
    src/cli.cpp
    src/csv_export.cpp
    src/history_remote.cpp
    src/session.cpp
    src/session_control.cpp
    src/session_history.cpp
    src/session_jobs.cpp
    src/session_live.cpp
    src/session_stream.cpp
    src/session_video.cpp
    src/sources/daq_tcp.cpp
    src/sources/log_replay.cpp
    src/sources/pcan_source.cpp
    src/sources/source_hub.cpp
    src/sources/synthetic.cpp
    src/track_catalog.cpp
    src/video_relay.cpp
)
target_include_directories(photon_daemon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(photon_daemon PUBLIC photon-core photon-export-lib)
if(WIN32)
    target_link_libraries(photon_daemon PRIVATE ws2_32)
endif()

file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets" PHOTON_ASSETS_DIR_NORM)
target_compile_definitions(photon_daemon PRIVATE PHOTON_ASSETS_DIR="${PHOTON_ASSETS_DIR_NORM}")

photon_apply_warnings(photon_daemon)

add_executable(photon-backend main.cpp)
target_link_libraries(photon-backend PRIVATE photon_daemon)
if(PHOTON_SHIP_DIR)
    set_target_properties(photon-backend PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PHOTON_SHIP_DIR}"
    )
endif()

add_custom_command(TARGET photon-backend POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:photon-backend>/assets"
)

photon_apply_warnings(photon-backend)

add_executable(photon-index
    ${CMAKE_SOURCE_DIR}/tools/photon-index/main.cpp
    ${CMAKE_SOURCE_DIR}/tools/photon-index/live_fanout.cpp
)
target_include_directories(photon-index PRIVATE ${CMAKE_SOURCE_DIR}/tools/photon-index)
target_link_libraries(photon-index PRIVATE photon-core)
if(WIN32)
    target_link_libraries(photon-index PRIVATE ws2_32)
endif()
if(CMAKE_BUILD_TYPE)
    target_compile_definitions(photon-index PRIVATE PHOTON_BUILD_TYPE=${CMAKE_BUILD_TYPE})
endif()
if(PHOTON_SHIP_DIR)
    set_target_properties(photon-index PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PHOTON_SHIP_DIR}"
    )
endif()
photon_apply_warnings(photon-index)
