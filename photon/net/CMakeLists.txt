set(PHOTON_NET_SOURCES
    src/sha1.cpp
    src/base64.cpp
    src/ws_handshake.cpp
)
if(NOT EMSCRIPTEN)
    list(APPEND PHOTON_NET_SOURCES
        src/net.cpp
        src/websocket.cpp
    )
endif()

add_library(photon_net STATIC ${PHOTON_NET_SOURCES})
target_include_directories(photon_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(photon_net PUBLIC photon_base photon_wire)
if(NOT EMSCRIPTEN AND WIN32)
    target_link_libraries(photon_net PUBLIC ws2_32)
endif()
photon_apply_warnings(photon_net)
