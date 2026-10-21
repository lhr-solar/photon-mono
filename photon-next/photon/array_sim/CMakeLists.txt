option(PHOTON_REQUIRE_ARRAY_SIM "Fail configuration if Embree 4 is unavailable" OFF)
find_package(embree 4 CONFIG QUIET)
if(PHOTON_REQUIRE_ARRAY_SIM AND NOT embree_FOUND)
    message(FATAL_ERROR "Array Sim requires Embree 4. Configure with the vcpkg toolchain.")
endif()

add_library(photon_array_sim STATIC src/array_sim.cpp)
target_include_directories(photon_array_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(WIN32)
    target_compile_definitions(photon_array_sim PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(embree_FOUND)
    target_sources(photon_array_sim PRIVATE
        src/stl_mesh.cpp
        src/trace_scene.cpp
        src/simulation.cpp
    )
    target_link_libraries(photon_array_sim PRIVATE embree)
    target_compile_definitions(photon_array_sim PRIVATE PHOTON_ARRAY_SIM_EMBREE=1)
else()
    message(WARNING "Embree 4 not found: Photon Array Sim will report unavailable. Configure with the vcpkg toolchain to enable it.")
endif()
photon_apply_warnings(photon_array_sim)
