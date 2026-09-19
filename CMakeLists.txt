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
endfunction()

if(PHOTON_FRONTEND)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR
            "PHOTON_FRONTEND=ON requires the Emscripten toolchain.\n"
            "Use frontend/scripts/build.ps1 or:\n"
            "  emcmake cmake -S . -B .artifacts-web -G Ninja -DPHOTON_FRONTEND=ON -DBUILD_TESTING=OFF")
    endif()
    add_subdirectory(core)
    add_subdirectory(frontend)
else()
    add_subdirectory(core)
    add_subdirectory(tools/photon-export)
    add_subdirectory(backend)
    if(PHOTON_NATIVE_UI)
        add_subdirectory(frontend)
    endif()
    include(CTest)
    if(BUILD_TESTING)
        add_subdirectory(tests)
    endif()
endif()
