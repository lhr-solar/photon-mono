set(PHOTON_NET_SOURCES
    sha1.cpp
    base64.cpp
    ws_handshake.cpp
)
if(NOT EMSCRIPTEN)
    list(APPEND PHOTON_NET_SOURCES
        net.cpp
        websocket.cpp
    )
endif()

add_library(photon_net STATIC ${PHOTON_NET_SOURCES})
target_include_directories(photon_net PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(photon_net PUBLIC photon_base photon_wire)
if(NOT EMSCRIPTEN AND WIN32)
    target_link_libraries(photon_net PUBLIC ws2_32)
endif()
photon_apply_warnings(photon_net)
