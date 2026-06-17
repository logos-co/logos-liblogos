# Logos Core Tests

# Get SDK root from parent scope
set(_default_sdk_root "${CMAKE_SOURCE_DIR}/vendor/logos-cpp-sdk")
if(NOT DEFINED LOGOS_CPP_SDK_ROOT OR LOGOS_CPP_SDK_ROOT STREQUAL "")
  set(LOGOS_CPP_SDK_ROOT "${_default_sdk_root}")
endif()
get_filename_component(LOGOS_CPP_SDK_ROOT "${LOGOS_CPP_SDK_ROOT}" ABSOLUTE)

add_executable(logos_core_tests
    test_app_lifecycle.cpp
    test_access_policy.cpp
    test_module_manager.cpp
    test_process_stats.cpp
    test_dependency_resolver.cpp
    test_subprocess_manager.cpp
    test_composite_module_loader.cpp
    test_module_name_validation.cpp
    test_module_loader_registry.cpp
    test_module_loader_abstraction.cpp
    test_protocol_gate.cpp
)

# Imported container/loader targets the tests drive via SubprocessManager /
# CompositeModuleLoader: logos_container (contract), logos_container_subprocess
# (parent container), logos_module_loader (format-loader contract),
# logos_module_loader_qt (QtPluginFormatLoader lib). Pulled in transitively via
# logos_core but linked explicitly for the helpers the tests call. The Qt-plugin
# loader tests and the TokenSource tests now live in logos-module-loader-qt.
target_link_libraries(logos_core_tests PRIVATE
    logos_core
    process_stats
    logos_container
    logos_container_subprocess
    logos_module_loader
    logos_module_loader_qt
    GTest::gtest
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Network
)

target_include_directories(logos_core_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/logos_core
)

# Propagate portable build flag to tests so manifest variant keys match
if(LOGOS_PORTABLE_BUILD)
    target_compile_definitions(logos_core_tests PRIVATE LOGOS_PORTABLE_BUILD=1)
endif()

# Add process-stats include directory
if(DEFINED PROCESS_STATS_ROOT AND EXISTS "${PROCESS_STATS_ROOT}/include")
  target_include_directories(logos_core_tests PRIVATE ${PROCESS_STATS_ROOT}/include)
endif()

# Add SDK include directories (same layering as src/CMakeLists.txt:
# Qt-free base headers + qt-sdk developer layer + protocol layer)
if(EXISTS "${LOGOS_CPP_SDK_ROOT}/include")
  target_include_directories(logos_core_tests PRIVATE 
    ${LOGOS_CPP_SDK_ROOT}/include
    ${LOGOS_CPP_SDK_ROOT}/include/cpp
  )
else()
  target_include_directories(logos_core_tests PRIVATE ${LOGOS_CPP_SDK_ROOT}/cpp)
endif()
if(DEFINED LOGOS_QT_SDK_ROOT AND EXISTS "${LOGOS_QT_SDK_ROOT}/include")
  target_include_directories(logos_core_tests PRIVATE
    ${LOGOS_QT_SDK_ROOT}/include
    ${LOGOS_QT_SDK_ROOT}/include/cpp
    ${LOGOS_QT_SDK_ROOT}/include/core
  )
endif()
if(DEFINED LOGOS_PROTOCOL_ROOT AND EXISTS "${LOGOS_PROTOCOL_ROOT}/include")
  target_include_directories(logos_core_tests PRIVATE
    ${LOGOS_PROTOCOL_ROOT}/include
    ${LOGOS_PROTOCOL_ROOT}/include/cpp
  )
endif()

gtest_discover_tests(logos_core_tests)
