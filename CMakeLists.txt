cmake_minimum_required(VERSION 3.14)
project(SimplePluginExample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Qt packages
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core RemoteObjects)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core RemoteObjects)

# Set output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

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
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # iOS: only install the static library
    install(TARGETS logos_core
        ARCHIVE DESTINATION lib
    )
else()
    # Desktop: install library and executables
    install(TARGETS logos_core logoscore logos_host
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
        ARCHIVE DESTINATION lib
    )
endif()

# Install headers
install(FILES src/common/interface.h src/logos_core/logos_core.h
    DESTINATION include
)
