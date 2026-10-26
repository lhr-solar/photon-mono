cmake_minimum_required(VERSION 3.25 FATAL_ERROR)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

if(NOT DEFINED CMAKE_BUILD_TYPE AND NOT DEFINED CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_SUPPRESS_REGENERATION ON)

option(PHOTON_FRONTEND "Build Dear ImGui WASM frontend (requires Emscripten)" OFF)
option(PHOTON_NATIVE_UI "Build desktop ImGui executable (photon-ui)" ON)

if(NOT CMAKE_TOOLCHAIN_FILE AND NOT DEFINED CMAKE_C_COMPILER)
    find_program(CMAKE_C_COMPILER clang REQUIRED)
endif()

if(NOT CMAKE_TOOLCHAIN_FILE AND NOT DEFINED CMAKE_CXX_COMPILER)
    find_program(CMAKE_CXX_COMPILER clang++ REQUIRED)
endif()

if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:DebugDLL>")
endif()

project(photon-next VERSION 0.1.0 LANGUAGES C CXX)

# Binaries stay in the build tree. Runnable bundles come from install components,
# which the repo's photon build installs into out/dist (see README):
#   server   photon-index, assets/dbc
#   client   photon-backend, assets/dbc, assets/tracks
#   desktop  photon-ui, assets/fonts, assets/models, assets/maps3d.*
#   web      ui/ (the page and its models)
#   tools    photon-export
#   symbols  .pdb files for the above
function(photon_install_symbols target)
    if(WIN32 AND NOT EMSCRIPTEN)
        # Release photon-backend links without /DEBUG, so a missing .pdb is fine.
        install(FILES "$<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_BASE_NAME:${target}>.pdb"
            DESTINATION . COMPONENT symbols OPTIONAL)
    endif()
endfunction()

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING
        "photon-next is developed with clang. Current compilers: "
        "C=${CMAKE_C_COMPILER_ID} CXX=${CMAKE_CXX_COMPILER_ID}")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(photon_apply_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra -Wpedantic>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
    )
    if(WIN32)
        # getenv, sscanf, and strncat are standard C; the CRT's *_s replacements
        # are Microsoft-only and do not exist under Emscripten.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(EMSCRIPTEN)
        # EM_ASM passes its arguments as $0, $1, which -Wpedantic reads as identifiers.
        target_compile_options(${target} PRIVATE -Wno-dollar-in-identifier-extension)
    endif()
endfunction()

find_program(CLANG_FORMAT_EXE NAMES clang-format HINTS "C:/Program Files/LLVM/bin")
if(CLANG_FORMAT_EXE)
    file(GLOB_RECURSE PHOTON_FORMAT_FILES
        "${CMAKE_SOURCE_DIR}/photon/*.cpp"
        "${CMAKE_SOURCE_DIR}/photon/*.hpp"
        "${CMAKE_SOURCE_DIR}/photon/*.h"
    )
    file(GLOB PHOTON_FORMAT_TEST_FILES
        "${CMAKE_SOURCE_DIR}/tests/*.cpp"
        "${CMAKE_SOURCE_DIR}/tests/*.hpp"
    )
    file(GLOB PHOTON_FORMAT_EXPORT_FILES
        "${CMAKE_SOURCE_DIR}/tools/photon-export/*.cpp"
        "${CMAKE_SOURCE_DIR}/tools/photon-export/*.hpp"
    )
    file(GLOB PHOTON_FORMAT_INDEX_FILES
        "${CMAKE_SOURCE_DIR}/tools/photon-index/*.cpp"
        "${CMAKE_SOURCE_DIR}/tools/photon-index/*.hpp"
    )
    list(APPEND PHOTON_FORMAT_FILES
        ${PHOTON_FORMAT_TEST_FILES}
        ${PHOTON_FORMAT_EXPORT_FILES}
        ${PHOTON_FORMAT_INDEX_FILES}
    )
    add_custom_target(format
        COMMAND "${CLANG_FORMAT_EXE}" -i ${PHOTON_FORMAT_FILES}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Formatting photon-next source files"
        VERBATIM
    )
    add_custom_target(format-check
        COMMAND "${CLANG_FORMAT_EXE}" --dry-run --Werror ${PHOTON_FORMAT_FILES}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Checking photon-next source formatting"
        VERBATIM
    )
else()
    message(STATUS "clang-format not found; format and format-check targets skipped")
endif()

if(PHOTON_FRONTEND)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR
            "PHOTON_FRONTEND=ON requires the Emscripten toolchain.\n"
            "Use web/scripts/build.ps1, or load emsdk_env and run:\n"
            "  cmake --preset web-release")
    endif()
    add_subdirectory(photon)
    add_subdirectory(photon/ui)
else()
    # Before tools/photon-export, which registers its own test.
    include(CTest)
    # photon-index reads the DBC; photon-backend reads the DBCs and the track catalog.
    install(DIRECTORY assets/dbc DESTINATION assets COMPONENT server)
    install(DIRECTORY assets/dbc assets/tracks DESTINATION assets COMPONENT client)
    add_subdirectory(photon)
    add_subdirectory(tools/photon-export)
    add_subdirectory(photon/daemon)
    if(PHOTON_NATIVE_UI)
        add_subdirectory(photon/ui)
    endif()
    if(BUILD_TESTING)
        add_subdirectory(tests)
    endif()
endif()
