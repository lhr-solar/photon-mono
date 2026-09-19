include(FetchContent)

set(PHOTON_EXTERNAL_DIR "${CMAKE_SOURCE_DIR}/.external")
set(PHOTON_IMGUI_TAG "v1.92.9-docking")
set(PHOTON_IMPLOT_TAG "v0.17")

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG ${PHOTON_IMGUI_TAG}
    GIT_SHALLOW TRUE
    SOURCE_DIR ${PHOTON_EXTERNAL_DIR}/imgui
    SOURCE_SUBDIR do_not_add_imgui_cmake
)
FetchContent_Declare(implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG ${PHOTON_IMPLOT_TAG}
    GIT_SHALLOW TRUE
    SOURCE_DIR ${PHOTON_EXTERNAL_DIR}/implot
    SOURCE_SUBDIR do_not_add_implot_cmake
)
FetchContent_MakeAvailable(imgui implot)

message(STATUS "photon-frontend imgui ${PHOTON_IMGUI_TAG} -> ${imgui_SOURCE_DIR}")
message(STATUS "photon-frontend implot ${PHOTON_IMPLOT_TAG} -> ${implot_SOURCE_DIR}")

# tinygltf: vendored Photon copy (header v2.4.2) into .external/tinygltf.
# Do not point at F:\lhr\Photon at build time. Fetch the same tree if missing.
set(PHOTON_TINYGLTF_DIR "${PHOTON_EXTERNAL_DIR}/tinygltf")
if(NOT EXISTS "${PHOTON_TINYGLTF_DIR}/tiny_gltf.h")
    FetchContent_Declare(tinygltf
        GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
        GIT_TAG v2.8.0
        GIT_SHALLOW TRUE
        SOURCE_DIR ${PHOTON_TINYGLTF_DIR}
        SOURCE_SUBDIR do_not_add_tinygltf_cmake
    )
    FetchContent_MakeAvailable(tinygltf)
endif()
message(STATUS "photon-frontend tinygltf -> ${PHOTON_TINYGLTF_DIR}")

add_library(photon-tinygltf STATIC
    src/gl3d/tinygltf_impl.cpp
)
target_include_directories(photon-tinygltf PUBLIC ${PHOTON_TINYGLTF_DIR})
target_compile_definitions(photon-tinygltf PUBLIC
    TINYGLTF_NOEXCEPTION
    TINYGLTF_NO_STB_IMAGE_WRITE
    JSON_NOEXCEPTION
)
# Third-party header is not -Wpedantic clean; isolate it from photon_apply_warnings.

set(PHOTON_IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)
set(PHOTON_UI_SOURCES
    src/main.cpp
    src/app.cpp
    src/arena.cpp
    src/page_url.cpp
    src/client.cpp
    src/signal_map.cpp
    src/alarms.cpp
    src/theme.cpp
    src/tile_cache.cpp
    src/analysis_runtime.cpp
    src/analysis_workbook.cpp
    src/analysis_eval.cpp
    src/tile_store.cpp
    src/custom_view.cpp
    src/gg_plot.cpp
    src/log_drawer.cpp
    src/video_decoder.cpp
    src/video_hud.cpp
    src/widgets.cpp
    src/gl3d/renderer.cpp
    src/gl3d/model.cpp
    src/gl3d/scene.cpp
    src/screens/analysis.cpp
    src/screens/can.cpp
    src/screens/dashboard.cpp
    src/screens/dynamics.cpp
    src/screens/live.cpp
    src/screens/network.cpp
    src/screens/pit.cpp
    src/screens/plots.cpp
    src/screens/scene3d.cpp
    src/screens/stubs.cpp
    src/screens/tracksim.cpp
)

set(PHOTON_MODELS_DIR "${CMAKE_SOURCE_DIR}/assets/models")
set(PHOTON_FONTS_DIR "${CMAKE_SOURCE_DIR}/assets/fonts")
if(NOT EXISTS "${PHOTON_FONTS_DIR}/Inter_28pt-Regular.ttf")
    message(FATAL_ERROR "missing ${PHOTON_FONTS_DIR}/Inter_28pt-Regular.ttf")
endif()
if(NOT EXISTS "${PHOTON_FONTS_DIR}/TablerIcons.ttf")
    message(FATAL_ERROR "missing ${PHOTON_FONTS_DIR}/TablerIcons.ttf")
endif()

if(NOT EMSCRIPTEN)
    find_package(SDL2 QUIET)
    if(NOT SDL2_FOUND AND NOT TARGET SDL2::SDL2 AND NOT TARGET SDL2::SDL2-static)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST OFF CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(sdl2
            URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.8/SDL2-2.32.8.tar.gz
            SOURCE_DIR ${PHOTON_EXTERNAL_DIR}/sdl2
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(sdl2)
    endif()
    if(TARGET SDL2::SDL2-static)
        set(PHOTON_SDL2_TARGET SDL2::SDL2-static)
    elseif(TARGET SDL2::SDL2)
        set(PHOTON_SDL2_TARGET SDL2::SDL2)
    elseif(TARGET SDL2-static)
        set(PHOTON_SDL2_TARGET SDL2-static)
    else()
        message(FATAL_ERROR "SDL2 target not found after find_package/FetchContent")
    endif()
    message(STATUS "photon-ui SDL2 -> ${PHOTON_SDL2_TARGET}")
endif()

add_library(photon-imgui STATIC ${PHOTON_IMGUI_SOURCES})
target_include_directories(photon-imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${implot_SOURCE_DIR}
)
if(EMSCRIPTEN)
    target_compile_definitions(photon-imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
    target_compile_options(photon-imgui PUBLIC "SHELL:--use-port=sdl2")
    target_link_options(photon-imgui PUBLIC "SHELL:--use-port=sdl2")
else()
    target_link_libraries(photon-imgui PUBLIC ${PHOTON_SDL2_TARGET})
    # clang + MSVC ABI: SDL_endian.h redefines the _m_prefetch builtin.
    target_compile_definitions(photon-imgui PUBLIC __PRFCHWINTRIN_H)
endif()

if(EMSCRIPTEN)
add_executable(photon-frontend
    ${PHOTON_UI_SOURCES}
    src/idb_tile_store.cpp
)

target_include_directories(photon-frontend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/backend/include
)

target_link_libraries(photon-frontend PRIVATE photon-core photon-imgui photon-tinygltf)

target_compile_options(photon-frontend PRIVATE "SHELL:--use-port=sdl2")

# --preload-file packs assets into photon.data and mounts them on MEMFS.
# Fonts stay out of photon.wasm (cacheable sidecar). TablerIcons.ttf is ~2.8 MB
# because the PUA range is almost the whole file; subsetting saved little.
target_link_options(photon-frontend PRIVATE
    "SHELL:--use-port=sdl2"
    "SHELL:-sUSE_WEBGL2=1"
    "SHELL:-sMIN_WEBGL_VERSION=2"
    "SHELL:-sMAX_WEBGL_VERSION=2"
    "SHELL:-sFULL_ES3=1"
    "SHELL:-sFORCE_FILESYSTEM=1"
    "SHELL:-sALLOW_MEMORY_GROWTH=1"
    "SHELL:-sINITIAL_MEMORY=67108864"
    "SHELL:-sNO_EXIT_RUNTIME=1"
    "SHELL:-sENVIRONMENT=web"
    "SHELL:-sEXPORTED_FUNCTIONS=['_main','_photon_on_view_import','_photon_on_analysis_import']"
    "SHELL:-sEXPORTED_RUNTIME_METHODS=['ccall','stringToUTF8']"
    "SHELL:-lwebsocket.js"
    "SHELL:--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/shell.html"
    "SHELL:--preload-file=${PHOTON_FONTS_DIR}/Inter_28pt-Regular.ttf@/fonts/Inter_28pt-Regular.ttf"
    "SHELL:--preload-file=${PHOTON_FONTS_DIR}/TablerIcons.ttf@/fonts/TablerIcons.ttf"
)

set_target_properties(photon-frontend PROPERTIES
    OUTPUT_NAME photon
    SUFFIX ".html"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shell.html"
)

# Recovered cars/track stay as HTTP static files. Do not --preload-file them
# (photon.data would jump from ~3 MB to 20+ MB).
set(PHOTON_WEB_MODELS_DIR "${CMAKE_BINARY_DIR}/models")
set(PHOTON_WEB_GLBS
    "${PHOTON_MODELS_DIR}/new_dyn.glb"
    "${PHOTON_MODELS_DIR}/newCar.glb"
    "${PHOTON_MODELS_DIR}/s26_simple_track.glb"
)
set(PHOTON_WEB_GLBS_EXIST TRUE)
foreach(_glb IN LISTS PHOTON_WEB_GLBS)
    if(NOT EXISTS "${_glb}")
        set(PHOTON_WEB_GLBS_EXIST FALSE)
    endif()
endforeach()
if(PHOTON_WEB_GLBS_EXIST)
    add_custom_command(TARGET photon-frontend POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PHOTON_WEB_MODELS_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PHOTON_WEB_GLBS}
            "${PHOTON_WEB_MODELS_DIR}/"
        COMMENT "Copy recovered GLBs to ${PHOTON_WEB_MODELS_DIR} (runtime fetch, not MEMFS)"
    )
else()
    message(STATUS "photon-frontend skipping GLB copy (missing files under ${PHOTON_MODELS_DIR})")
endif()

photon_apply_warnings(photon-frontend)

message(STATUS "photon-frontend -> ${CMAKE_BINARY_DIR}/photon.html")
else()
    add_library(photon-ws STATIC
        ${CMAKE_SOURCE_DIR}/backend/src/net.cpp
        ${CMAKE_SOURCE_DIR}/backend/src/websocket.cpp
    )
    target_include_directories(photon-ws PUBLIC ${CMAKE_SOURCE_DIR}/backend/include)
    target_link_libraries(photon-ws PUBLIC photon-core)
    if(WIN32)
        target_link_libraries(photon-ws PUBLIC ws2_32)
    endif()

    add_executable(photon-ui
        ${PHOTON_UI_SOURCES}
        src/platform.cpp
        src/gl_loader.cpp
        src/native_maps.cpp
        src/native_video.cpp
    )
    target_include_directories(photon-ui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/backend/include
    )
    file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets" PHOTON_ASSETS_DIR_NORM)
    target_compile_definitions(photon-ui PRIVATE
        PHOTON_ASSETS_DIR="${PHOTON_ASSETS_DIR_NORM}"
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    target_compile_options(photon-ui PRIVATE
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-g>
        $<$<AND:$<CXX_COMPILER_ID:Clang>,$<PLATFORM_ID:Windows>>:-gcodeview>
    )
    target_link_options(photon-ui PRIVATE
        $<$<AND:$<CXX_COMPILER_ID:Clang>,$<PLATFORM_ID:Windows>>:LINKER:/DEBUG>
    )
    target_link_libraries(photon-ui PRIVATE
        photon-core
        photon-imgui
        photon-tinygltf
        photon-ws
        ${PHOTON_SDL2_TARGET}
    )
    if(WIN32)
        set(PHOTON_WEBVIEW2_VER "1.0.2849.39")
        set(PHOTON_WEBVIEW2_DIR "${PHOTON_EXTERNAL_DIR}/webview2")
        set(PHOTON_WEBVIEW2_HEADER "${PHOTON_WEBVIEW2_DIR}/build/native/include/WebView2.h")
        if(NOT EXISTS "${PHOTON_WEBVIEW2_HEADER}")
            set(PHOTON_WEBVIEW2_NUPKG "${CMAKE_BINARY_DIR}/webview2.nupkg")
            message(STATUS "photon-ui fetching WebView2 ${PHOTON_WEBVIEW2_VER}")
            file(DOWNLOAD
                "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${PHOTON_WEBVIEW2_VER}"
                "${PHOTON_WEBVIEW2_NUPKG}"
                STATUS PHOTON_WEBVIEW2_DL
            )
            list(GET PHOTON_WEBVIEW2_DL 0 PHOTON_WEBVIEW2_DL_CODE)
            if(NOT PHOTON_WEBVIEW2_DL_CODE EQUAL 0)
                message(FATAL_ERROR "WebView2 download failed: ${PHOTON_WEBVIEW2_DL}")
            endif()
            file(REMOVE_RECURSE "${PHOTON_WEBVIEW2_DIR}")
            file(MAKE_DIRECTORY "${PHOTON_WEBVIEW2_DIR}")
            file(ARCHIVE_EXTRACT INPUT "${PHOTON_WEBVIEW2_NUPKG}" DESTINATION "${PHOTON_WEBVIEW2_DIR}")
        endif()
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(PHOTON_WEBVIEW2_LIB "${PHOTON_WEBVIEW2_DIR}/build/native/x64/WebView2LoaderStatic.lib")
        else()
            set(PHOTON_WEBVIEW2_LIB "${PHOTON_WEBVIEW2_DIR}/build/native/x86/WebView2LoaderStatic.lib")
        endif()
        target_include_directories(photon-ui PRIVATE
            "${PHOTON_WEBVIEW2_DIR}/build/native/include"
        )
        target_link_libraries(photon-ui PRIVATE
            opengl32
            dbghelp
            ole32
            oleaut32
            mfplat
            mf
            mfuuid
            wmcodecdspuuid
            version
            "${PHOTON_WEBVIEW2_LIB}"
        )
        message(STATUS "photon-ui WebView2 -> ${PHOTON_WEBVIEW2_DIR}")
    endif()
    set_target_properties(photon-ui PROPERTIES
        OUTPUT_NAME photon-ui
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    add_custom_command(TARGET photon-ui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:photon-ui>/assets"
        COMMENT "Copy Photon assets next to photon-ui"
    )
    photon_apply_warnings(photon-ui)
    message(STATUS "photon-ui -> ${CMAKE_BINARY_DIR}/photon-ui${CMAKE_EXECUTABLE_SUFFIX}")
endif()
