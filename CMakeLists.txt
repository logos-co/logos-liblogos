cmake_minimum_required(VERSION 3.14)
project(SimplePluginExample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Qt packages
# Network is needed explicitly by the token-exchange tests (QLocalServer /
# QLocalSocket); it is also pulled transitively by RemoteObjects in the
# runtime build, but we declare it here so the test target can link it
# explicitly without depending on that transitive resolution.
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network RemoteObjects)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network RemoteObjects)

# Find nlohmann_json
find_package(nlohmann_json REQUIRED)

# OpenSSL needed transitively by logos_sdk (plain-C++ TCP+SSL transport)
find_package(OpenSSL REQUIRED)

# Find CLI11
find_package(CLI11 REQUIRED)

# Find spdlog
find_package(spdlog REQUIRED)

# Set output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Add external library paths to build RPATH so test binaries can find them
if(DEFINED LOGOS_PACKAGE_MANAGER_ROOT)
    list(APPEND CMAKE_BUILD_RPATH "${LOGOS_PACKAGE_MANAGER_ROOT}/lib")
endif()

# Build src first to ensure logos_core is built before modules
add_subdirectory(src)

# GoogleTest setup
# Try to find GoogleTest via find_package first (for Nix and system installations)
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    # Fall back to FetchContent if GoogleTest is not found
    message(STATUS "GoogleTest not found via find_package, using FetchContent")
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    FetchContent_MakeAvailable(googletest)
else()
    message(STATUS "Using system GoogleTest")
endif()

enable_testing()
include(GoogleTest)

# Add tests subdirectory
add_subdirectory(tests)

# Install rules
install(TARGETS logos_core logos_host_qt
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
)

# Compatibility symlink: logos_host -> logos_host_qt so downstream consumers keep working
install(CODE "
  execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink
    logos_host_qt
    \${CMAKE_INSTALL_PREFIX}/bin/logos_host)
")

# Install headers
install(FILES src/logos_core/logos_core.h
    DESTINATION include
)
