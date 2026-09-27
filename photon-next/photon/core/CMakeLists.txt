set(PHOTON_CORE_SOURCES
    src/json.cpp
    src/tracksim.cpp
    src/protocol.cpp
    src/control.cpp
    src/canp.cpp
    src/dbc.cpp
    src/decode_buffer.cpp
    src/sha1.cpp
    src/base64.cpp
    src/ws_handshake.cpp
    src/tsdb.cpp
    src/tsdb_live.cpp
    src/vsd1.cpp
    src/video_index.cpp
    src/log_sessions.cpp
)
if(NOT EMSCRIPTEN)
    list(APPEND PHOTON_CORE_SOURCES
        src/net.cpp
        src/websocket.cpp
        src/pcan.cpp
    )
endif()

add_library(photon-core STATIC ${PHOTON_CORE_SOURCES})
target_include_directories(photon-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(NOT EMSCRIPTEN AND WIN32)
    target_link_libraries(photon-core PUBLIC ws2_32)
endif()
photon_apply_warnings(photon-core)
