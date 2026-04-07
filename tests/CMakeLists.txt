set(_default_sdk_root "${CMAKE_SOURCE_DIR}/vendor/logos-cpp-sdk")
if(NOT DEFINED LOGOS_CPP_SDK_ROOT OR LOGOS_CPP_SDK_ROOT STREQUAL "")
  set(LOGOS_CPP_SDK_ROOT "${_default_sdk_root}")
endif()
get_filename_component(LOGOS_CPP_SDK_ROOT "${LOGOS_CPP_SDK_ROOT}" ABSOLUTE)

add_executable(logos_core_tests
    test_app_lifecycle.cpp
    test_plugin_manager.cpp
    test_process_stats.cpp
)

target_link_libraries(logos_core_tests PRIVATE
    logos_core
    GTest::gtest
)

target_include_directories(logos_core_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/logos_core
    ${CMAKE_SOURCE_DIR}/src/logos_core/platform
)

if(LOGOS_PORTABLE_BUILD)
  target_compile_definitions(logos_core_tests PRIVATE LOGOS_PORTABLE_BUILD=1)
endif()

if(EXISTS "${LOGOS_CPP_SDK_ROOT}/lib" AND EXISTS "${LOGOS_CPP_SDK_ROOT}/include")
  target_include_directories(logos_core_tests PRIVATE
    ${LOGOS_CPP_SDK_ROOT}/include
    ${LOGOS_CPP_SDK_ROOT}/include/cpp
    ${LOGOS_CPP_SDK_ROOT}/include/core
  )
else()
  target_include_directories(logos_core_tests PRIVATE ${LOGOS_CPP_SDK_ROOT}/cpp)
endif()

gtest_discover_tests(logos_core_tests)
