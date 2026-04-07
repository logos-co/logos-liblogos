cmake_minimum_required(VERSION 3.14)
project(SimplePluginExample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED)
find_package(spdlog CONFIG QUIET)
if(NOT spdlog_FOUND)
  find_package(spdlog REQUIRED)
endif()
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core RemoteObjects)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core RemoteObjects)

find_package(nlohmann_json REQUIRED)
find_package(CLI11 REQUIRED)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(DEFINED LOGOS_PACKAGE_MANAGER_ROOT)
    list(APPEND CMAKE_BUILD_RPATH "${LOGOS_PACKAGE_MANAGER_ROOT}/lib")
endif()

add_subdirectory(src)

find_package(GTest QUIET)

if(NOT GTest_FOUND)
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

add_subdirectory(tests)

install(TARGETS logos_core logos_host
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
)

install(FILES src/logos_core/logos_core.h
    DESTINATION include
)
