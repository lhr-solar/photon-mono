add_library(photon-core STATIC
    src/base64.cpp
    src/canp.cpp
    src/control.cpp
    src/dbc.cpp
    src/json.cpp
    src/log_sessions.cpp
    src/pcan.cpp
    src/protocol.cpp
    src/sha1.cpp
    src/tracksim.cpp
    src/tsdb.cpp
    src/tsdb_live.cpp
    src/video_index.cpp
    src/vsd1.cpp
    src/ws_handshake.cpp
)

target_include_directories(photon-core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

photon_apply_warnings(photon-core)
