# Logos Core Tests

# Get SDK root from parent scope
set(_default_sdk_root "${CMAKE_SOURCE_DIR}/vendor/logos-cpp-sdk")
if(NOT DEFINED LOGOS_CPP_SDK_ROOT OR LOGOS_CPP_SDK_ROOT STREQUAL "")
  set(LOGOS_CPP_SDK_ROOT "${_default_sdk_root}")
endif()
get_filename_component(LOGOS_CPP_SDK_ROOT "${LOGOS_CPP_SDK_ROOT}" ABSOLUTE)

add_executable(logos_core_tests
    test_app_lifecycle.cpp
    test_module_manager.cpp
    test_process_stats.cpp
    test_dependency_resolver.cpp
    test_subprocess_manager.cpp
    test_token_exchange.cpp
    test_runtime_registry.cpp
    test_module_runtime_abstraction.cpp
    # qt_test_adapter.h calls QtTokenReceiver::receiveAuthToken; compile the
    # implementation unit directly into the test binary (it lives in logos_host_qt,
    # which is an executable, not a linkable library).
    ${CMAKE_SOURCE_DIR}/src/runtimes/runtime_qt/qt/qt_token_receiver.cpp
)

target_link_libraries(logos_core_tests PRIVATE
    logos_core
    process_stats
    GTest::gtest
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Network
)

target_include_directories(logos_core_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/logos_core
    ${CMAKE_SOURCE_DIR}/src/runtimes
    ${CMAKE_SOURCE_DIR}/src/runtimes/runtime_qt
)

# Propagate portable build flag to tests so manifest variant keys match
if(LOGOS_PORTABLE_BUILD)
    target_compile_definitions(logos_core_tests PRIVATE LOGOS_PORTABLE_BUILD=1)
endif()

# Add process-stats include directory
if(DEFINED PROCESS_STATS_ROOT AND EXISTS "${PROCESS_STATS_ROOT}/include")
  target_include_directories(logos_core_tests PRIVATE ${PROCESS_STATS_ROOT}/include)
endif()

# Add SDK include directory (same logic as src/CMakeLists.txt)
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
