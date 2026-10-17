# A Basecamp-shaped process built the way an app consumes liblogos: the
# installed package, with the Qt host runtime linked ahead of core as Basecamp
# links it, so both protocol libraries are loaded in that order.
cmake_minimum_required(VERSION 3.16)
project(QtEmbedderTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core RemoteObjects)
find_package(GTest REQUIRED)
find_package(logos-protocol REQUIRED CONFIG
    PATHS ${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol NO_DEFAULT_PATH)
find_package(logos-qt-host REQUIRED CONFIG
    PATHS ${LOGOS_QT_HOST_ROOT}/lib/cmake/logos-qt-host NO_DEFAULT_PATH)
find_library(LOGOS_CORE_LIB logos_core PATHS ${LOGOS_LIBLOGOS_ROOT}/lib NO_DEFAULT_PATH REQUIRED)

add_executable(qt_embedder_tests test_qt_embedder_tokens.cpp)
target_include_directories(qt_embedder_tests PRIVATE ${LOGOS_LIBLOGOS_ROOT}/include)
target_link_libraries(qt_embedder_tests PRIVATE
    logos-qt-host::logos_qt_host_shared
    ${LOGOS_CORE_LIB}
    Qt6::Core
    GTest::gtest
)
set_target_properties(qt_embedder_tests PROPERTIES
    INSTALL_RPATH "${LOGOS_QT_HOST_ROOT}/lib;${LOGOS_PROTOCOL_ROOT}/lib;${LOGOS_LIBLOGOS_ROOT}/lib"
    BUILD_WITH_INSTALL_RPATH TRUE
)
