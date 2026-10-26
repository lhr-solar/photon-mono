add_library(photon-export-lib STATIC
    export.cpp
    parquet.cpp
)
target_include_directories(photon-export-lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(photon-export-lib PUBLIC photon-core)
photon_apply_warnings(photon-export-lib)

add_executable(photon-export main.cpp)
target_link_libraries(photon-export PRIVATE photon-export-lib)
if(WIN32)
    # GetProcessMemoryInfo, for the peak-working-set line in the report.
    target_link_libraries(photon-export PRIVATE psapi)
endif()
photon_apply_warnings(photon-export)
install(TARGETS photon-export RUNTIME DESTINATION . COMPONENT tools)
photon_install_symbols(photon-export)

if(BUILD_TESTING)
    add_executable(photon-export-tests test_export.cpp)
    target_link_libraries(photon-export-tests PRIVATE photon-export-lib)
    target_include_directories(photon-export-tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    photon_apply_warnings(photon-export-tests)
    add_test(NAME photon-export COMMAND photon-export-tests)
endif()
