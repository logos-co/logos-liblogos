# Logos Core Tests

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
    test_dependency_gate.cpp
    test_module_state_observer.cpp
    test_expected_exit_marks.cpp
    test_load_verdict.cpp
    test_load_concurrency.cpp
)

# Imported container/loader targets the tests drive via SubprocessManager /
# CompositeModuleLoader: logos_container + logos_module_loader (the contracts),
# and the build-selected implementation targets LogosContainerImpl::impl /
# LogosFormatLoaderImpl::impl (default subprocess + qt — the test shim
# subprocess_manager.h includes their concrete headers, which the impl configs'
# include dirs provide). The find_package calls are repeated here because the
# imported targets created in src/ are not visible in this sibling directory;
# each config guards with `if(NOT TARGET ...)` so this is safe.
find_package(LogosContainerImpl REQUIRED)
find_package(LogosFormatLoaderImpl REQUIRED)
target_link_libraries(logos_core_tests PRIVATE
    logos_core
    process_stats
    logos_container
    LogosContainerImpl::impl
    logos_module_loader
    LogosFormatLoaderImpl::impl
    GTest::gtest
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

if(DEFINED LOGOS_PROTOCOL_ROOT AND EXISTS "${LOGOS_PROTOCOL_ROOT}/include")
  target_include_directories(logos_core_tests PRIVATE
    ${LOGOS_PROTOCOL_ROOT}/include
    ${LOGOS_PROTOCOL_ROOT}/include/cpp
  )
endif()

# Sidecar-backed fixture binaries for object-form dependency metadata. The
# registry follows the same production path used by current module packages,
# without compiling Qt into the test process.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/../lib")
foreach(_fixture dep_range_fixture dep_malformed_fixture)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/../lib/${_fixture}_plugin.fixture" "fixture\n")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${_fixture}.json"
        "${CMAKE_CURRENT_BINARY_DIR}/../lib/${_fixture}_plugin.metadata.json"
        COPYONLY)
endforeach()

gtest_discover_tests(logos_core_tests DISCOVERY_MODE PRE_TEST)
