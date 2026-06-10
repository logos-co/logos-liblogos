set(CMAKE_AUTOMOC ON)

set(_default_sdk_root "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/logos-cpp-sdk")
if(NOT DEFINED LOGOS_CPP_SDK_ROOT OR LOGOS_CPP_SDK_ROOT STREQUAL "")
  set(LOGOS_CPP_SDK_ROOT "${_default_sdk_root}")
endif()

get_filename_component(LOGOS_CPP_SDK_ROOT "${LOGOS_CPP_SDK_ROOT}" ABSOLUTE)

# The SDK stack since the Qt split: logos-protocol (transports + lp_* C
# ABI), logos-qt-sdk (LogosAPI + provider glue — what this runtime's Qt
# side uses), logos-cpp-sdk (Qt-free std headers). The `logos_sdk` alias
# is kept so the rest of this file (and the host targets) stay unchanged;
# it now points at logos-qt-sdk, whose link interface chains
# logos-protocol and its OpenSSL/Boost/nlohmann deps.
if(NOT DEFINED LOGOS_PROTOCOL_ROOT AND DEFINED ENV{LOGOS_PROTOCOL_ROOT})
  set(LOGOS_PROTOCOL_ROOT "$ENV{LOGOS_PROTOCOL_ROOT}")
endif()
if(NOT DEFINED LOGOS_QT_SDK_ROOT AND DEFINED ENV{LOGOS_QT_SDK_ROOT})
  set(LOGOS_QT_SDK_ROOT "$ENV{LOGOS_QT_SDK_ROOT}")
endif()

if(EXISTS "${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol"
   AND EXISTS "${LOGOS_QT_SDK_ROOT}/lib/cmake/logos-qt-sdk")
  message(STATUS "Using logos-protocol at ${LOGOS_PROTOCOL_ROOT}")
  message(STATUS "Using logos-qt-sdk at ${LOGOS_QT_SDK_ROOT}")
  find_package(logos-protocol REQUIRED
    PATHS "${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol" NO_DEFAULT_PATH)
  find_package(logos-qt-sdk REQUIRED
    PATHS "${LOGOS_QT_SDK_ROOT}/lib/cmake/logos-qt-sdk" NO_DEFAULT_PATH)
  if(NOT TARGET logos_sdk)
    add_library(logos_sdk INTERFACE IMPORTED)
    target_link_libraries(logos_sdk INTERFACE logos-qt-sdk::logos_qt_sdk)
  endif()
else()
  message(FATAL_ERROR "logos-protocol / logos-qt-sdk not found. Set "
                      "LOGOS_PROTOCOL_ROOT and LOGOS_QT_SDK_ROOT to built packages.")
endif()

# Qt-free base SDK headers (logos_module_context.h etc.) — header-only.
if(EXISTS "${LOGOS_CPP_SDK_ROOT}/lib/cmake/logos-cpp-sdk")
  find_package(logos-cpp-sdk REQUIRED
    PATHS "${LOGOS_CPP_SDK_ROOT}/lib/cmake/logos-cpp-sdk" NO_DEFAULT_PATH)
  if(NOT TARGET logos_headers)
    add_library(logos_headers INTERFACE IMPORTED)
    target_link_libraries(logos_headers INTERFACE logos-cpp-sdk::logos_headers)
  endif()
  target_link_libraries(logos_sdk INTERFACE logos-cpp-sdk::logos_headers)
endif()

# Find logos-module library
if(DEFINED LOGOS_MODULE_ROOT AND EXISTS "${LOGOS_MODULE_ROOT}/lib")
  message(STATUS "Using logos-module at ${LOGOS_MODULE_ROOT}")
  
  find_library(LOGOS_MODULE_LIBRARY
    NAMES logos_module
    PATHS "${LOGOS_MODULE_ROOT}/lib"
    NO_DEFAULT_PATH
  )
  
  if(NOT LOGOS_MODULE_LIBRARY)
    message(FATAL_ERROR "logos_module library not found in ${LOGOS_MODULE_ROOT}/lib")
  endif()
  
  # Create an imported target for logos_module
  add_library(logos_module STATIC IMPORTED)
  set_target_properties(logos_module PROPERTIES
    IMPORTED_LOCATION "${LOGOS_MODULE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_MODULE_ROOT}/include"
  )
else()
  message(STATUS "LOGOS_MODULE_ROOT not set, logos-module will need to be provided")
endif()

# Find process-stats library
if(DEFINED PROCESS_STATS_ROOT AND EXISTS "${PROCESS_STATS_ROOT}/lib")
  message(STATUS "Using process-stats at ${PROCESS_STATS_ROOT}")

  find_library(PROCESS_STATS_LIBRARY
    NAMES process_stats
    PATHS "${PROCESS_STATS_ROOT}/lib"
    NO_DEFAULT_PATH
  )

  if(NOT PROCESS_STATS_LIBRARY)
    message(FATAL_ERROR "process_stats library not found in ${PROCESS_STATS_ROOT}/lib")
  endif()

  add_library(process_stats STATIC IMPORTED)
  set_target_properties(process_stats PROPERTIES
    IMPORTED_LOCATION "${PROCESS_STATS_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PROCESS_STATS_ROOT}/include"
  )
else()
  message(STATUS "PROCESS_STATS_ROOT not set, process-stats will need to be provided")
endif()


# Define the library sources
set(LOGOS_CORE_SOURCES
    logos_core/logos_core.cpp
    logos_core/logos_core.h
    logos_core/module_registry.cpp
    logos_core/module_registry.h
    logos_core/dependency_resolver.cpp
    logos_core/dependency_resolver.h
    logos_core/module_manager.cpp
    logos_core/module_manager.h
    logos_core/module_runtime.h
    logos_core/module_container.h
    logos_core/module_loader.h
    logos_core/composite_runtime.cpp
    logos_core/composite_runtime.h
    logos_core/runtime_registry.cpp
    logos_core/runtime_registry.h
    containers/subprocess/subprocess_container.cpp
    containers/subprocess/subprocess_container.h
    runtimes/runtime_qt/qt_plugin_runtime.cpp
    runtimes/runtime_qt/qt_plugin_runtime.h
)

# Define the logos host sources
set(LOGOS_HOST_SOURCES
    runtimes/runtime_qt/host/logos_host.cpp
    runtimes/runtime_qt/host/command_line_parser.cpp
    runtimes/runtime_qt/host/command_line_parser.h
    runtimes/runtime_qt/host/module_initializer.cpp
    runtimes/runtime_qt/host/module_initializer.h
    runtimes/runtime_qt/host/qt/qt_app.cpp
    runtimes/runtime_qt/host/qt/qt_app.h
    containers/subprocess/token_receiver.cpp
    containers/subprocess/token_receiver.h
)

# Create the logos core library
add_library(logos_core SHARED ${LOGOS_CORE_SOURCES})

# Set the LOGOS_CORE_LIBRARY definition for the library
target_compile_definitions(logos_core PRIVATE LOGOS_CORE_LIBRARY)

# Portable build: selects portable LGX variants instead of dev variants
option(LOGOS_PORTABLE_BUILD "Build for portable variant selection" OFF)
if(LOGOS_PORTABLE_BUILD)
    target_compile_definitions(logos_core PRIVATE LOGOS_PORTABLE_BUILD=1)
endif()

# Find Boost (header-only usage + Boost.Process/Asio)
find_package(Boost REQUIRED COMPONENTS process)

# Find logos-package-manager library
if(DEFINED LOGOS_PACKAGE_MANAGER_ROOT AND EXISTS "${LOGOS_PACKAGE_MANAGER_ROOT}/lib")
  message(STATUS "Using logos-package-manager at ${LOGOS_PACKAGE_MANAGER_ROOT}")

  find_library(LOGOS_PACKAGE_MANAGER_LIBRARY
    NAMES package_manager_lib
    PATHS "${LOGOS_PACKAGE_MANAGER_ROOT}/lib"
    NO_DEFAULT_PATH
  )

  if(NOT LOGOS_PACKAGE_MANAGER_LIBRARY)
    message(FATAL_ERROR "package_manager_lib not found in ${LOGOS_PACKAGE_MANAGER_ROOT}/lib")
  endif()

  add_library(package_manager_lib SHARED IMPORTED)
  set_target_properties(package_manager_lib PROPERTIES
    IMPORTED_LOCATION "${LOGOS_PACKAGE_MANAGER_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_PACKAGE_MANAGER_ROOT}/include"
  )
else()
  message(FATAL_ERROR "LOGOS_PACKAGE_MANAGER_ROOT not set or ${LOGOS_PACKAGE_MANAGER_ROOT}/lib does not exist")
endif()

# Link Qt libraries, SDK, module library, and package-manager library
target_link_libraries(logos_core PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
    logos_sdk
    logos_module
    process_stats
    package_manager_lib
    nlohmann_json::nlohmann_json
    Boost::process
    spdlog::spdlog
)

# Include directories for the library
target_include_directories(logos_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/logos_core
    ${CMAKE_CURRENT_SOURCE_DIR}/containers
    ${CMAKE_CURRENT_SOURCE_DIR}/runtimes
    ${Qt${QT_VERSION_MAJOR}_INCLUDE_DIRS}
)

# Add logos_module include directory if available
if(DEFINED LOGOS_MODULE_ROOT AND EXISTS "${LOGOS_MODULE_ROOT}/include")
  target_include_directories(logos_core PRIVATE
    ${LOGOS_MODULE_ROOT}/include
    ${LOGOS_MODULE_ROOT}/include/module_lib
  )
endif()

# Add process-stats include directory if available
if(DEFINED PROCESS_STATS_ROOT AND EXISTS "${PROCESS_STATS_ROOT}/include")
  target_include_directories(logos_core PRIVATE ${PROCESS_STATS_ROOT}/include)
endif()


# Add SDK include directory if using built SDK
if(EXISTS "${LOGOS_CPP_SDK_ROOT}/lib" AND EXISTS "${LOGOS_CPP_SDK_ROOT}/include")
  target_include_directories(logos_core PRIVATE 
    ${LOGOS_CPP_SDK_ROOT}/include
    ${LOGOS_CPP_SDK_ROOT}/include/cpp
    ${LOGOS_CPP_SDK_ROOT}/include/core
  )
else()
  target_include_directories(logos_core PRIVATE ${LOGOS_CPP_SDK_ROOT}/cpp)
endif()

# Set RPATH settings for the library
if(APPLE)
    set(_qt_runtime_paths "@loader_path")
    if(DEFINED Qt${QT_VERSION_MAJOR}_DIR)
        get_filename_component(_qt_lib_dir "${Qt${QT_VERSION_MAJOR}_DIR}/../.." REALPATH)
        list(APPEND _qt_runtime_paths "${_qt_lib_dir}")
    endif()
    list(REMOVE_DUPLICATES _qt_runtime_paths)

    set_target_properties(logos_core PROPERTIES
        INSTALL_RPATH "${_qt_runtime_paths}"
        INSTALL_NAME_DIR "@rpath"
        BUILD_WITH_INSTALL_NAME_DIR TRUE
        BUILD_WITH_INSTALL_RPATH TRUE)
else()
    set_target_properties(logos_core PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH TRUE)
endif()

# Create the logos host application
add_executable(logos_host_qt ${LOGOS_HOST_SOURCES})

# Link the logos host with Qt libraries and module library
target_link_libraries(logos_host_qt PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::RemoteObjects
    logos_sdk
    logos_module
    CLI11::CLI11
    spdlog::spdlog
)

# Include directories for the logos host application
target_include_directories(logos_host_qt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/runtimes/runtime_qt/host
    ${CMAKE_CURRENT_SOURCE_DIR}/containers/subprocess
    ${Qt${QT_VERSION_MAJOR}_INCLUDE_DIRS}
)

# Add logos_module include directory if available
if(DEFINED LOGOS_MODULE_ROOT AND EXISTS "${LOGOS_MODULE_ROOT}/include")
    target_include_directories(logos_host_qt PRIVATE
        ${LOGOS_MODULE_ROOT}/include
        ${LOGOS_MODULE_ROOT}/include/module_lib
    )
endif()

# Add SDK include directory if using built SDK
if(EXISTS "${LOGOS_CPP_SDK_ROOT}/lib" AND EXISTS "${LOGOS_CPP_SDK_ROOT}/include")
    target_include_directories(logos_host_qt PRIVATE
        ${LOGOS_CPP_SDK_ROOT}/include
        ${LOGOS_CPP_SDK_ROOT}/include/cpp
        ${LOGOS_CPP_SDK_ROOT}/include/core
    )
else()
    target_include_directories(logos_host_qt PRIVATE ${LOGOS_CPP_SDK_ROOT}/cpp)
endif()

# Set RPATH settings for the executable
if(APPLE)
    set(_qt_exec_runtime_paths "@loader_path/../lib")
    if(DEFINED Qt${QT_VERSION_MAJOR}_DIR)
        get_filename_component(_qt_exec_lib_dir "${Qt${QT_VERSION_MAJOR}_DIR}/../.." REALPATH)
        list(APPEND _qt_exec_runtime_paths "${_qt_exec_lib_dir}")
    endif()
    list(REMOVE_DUPLICATES _qt_exec_runtime_paths)

    set_target_properties(logos_host_qt PROPERTIES
        INSTALL_RPATH "${_qt_exec_runtime_paths}"
        BUILD_WITH_INSTALL_RPATH TRUE)
else()
    set_target_properties(logos_host_qt PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_WITH_INSTALL_RPATH TRUE)
endif()
