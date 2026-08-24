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

add_library(photon-imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)
target_include_directories(photon-imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${implot_SOURCE_DIR}
)
target_compile_definitions(photon-imgui PUBLIC
    IMGUI_IMPL_OPENGL_ES3
)
target_compile_options(photon-imgui PUBLIC "SHELL:--use-port=sdl2")
target_link_options(photon-imgui PUBLIC "SHELL:--use-port=sdl2")

add_executable(photon-frontend
    src/main.cpp
    src/app.cpp
    src/arena.cpp
    src/client.cpp
    src/signal_map.cpp
    src/alarms.cpp
    src/theme.cpp
    src/custom_view.cpp
    src/video_decoder.cpp
    src/widgets.cpp
    src/gl3d/renderer.cpp
    src/gl3d/model.cpp
    src/gl3d/scene.cpp
    src/screens/can.cpp
    src/screens/dynamics.cpp
    src/screens/live.cpp
    src/screens/network.cpp
    src/screens/pit.cpp
    src/screens/plots.cpp
    src/screens/scene3d.cpp
    src/screens/stubs.cpp
    src/screens/tracksim.cpp
)

target_include_directories(photon-frontend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(photon-frontend PRIVATE photon-core photon-imgui photon-tinygltf)

target_compile_options(photon-frontend PRIVATE "SHELL:--use-port=sdl2")

# --preload-file packs assets into photon.data and mounts them on MEMFS.
# Fonts stay out of photon.wasm (cacheable sidecar). TablerIcons.ttf is ~2.8 MB
# because the PUA range is almost the whole file; subsetting saved little.
set(PHOTON_MODELS_DIR "${CMAKE_SOURCE_DIR}/assets/models")
set(PHOTON_FONTS_DIR "${CMAKE_SOURCE_DIR}/assets/fonts")
if(NOT EXISTS "${PHOTON_FONTS_DIR}/Inter_28pt-Regular.ttf")
    message(FATAL_ERROR "missing ${PHOTON_FONTS_DIR}/Inter_28pt-Regular.ttf")
endif()
if(NOT EXISTS "${PHOTON_FONTS_DIR}/TablerIcons.ttf")
    message(FATAL_ERROR "missing ${PHOTON_FONTS_DIR}/TablerIcons.ttf")
endif()
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
    "SHELL:-sEXPORTED_FUNCTIONS=['_main','_photon_on_view_import']"
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
)

# Recovered cars/track stay as HTTP static files. Do not --preload-file them
# (photon.data would jump from ~3 MB to 20+ MB).
set(PHOTON_WEB_MODELS_DIR "${CMAKE_BINARY_DIR}/models")
add_custom_command(TARGET photon-frontend POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PHOTON_WEB_MODELS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PHOTON_MODELS_DIR}/new_dyn.glb"
        "${PHOTON_MODELS_DIR}/newCar.glb"
        "${PHOTON_MODELS_DIR}/s26_simple_track.glb"
        "${PHOTON_WEB_MODELS_DIR}/"
    COMMENT "Copy recovered GLBs to ${PHOTON_WEB_MODELS_DIR} (runtime fetch, not MEMFS)"
)

photon_apply_warnings(photon-frontend)

message(STATUS "photon-frontend -> ${CMAKE_BINARY_DIR}/photon.html")
