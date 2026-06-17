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

# Find logos-container (the Qt-free, header-only container contract:
# ModuleContainer interface + ModuleDescriptor/LoadedModuleHandle + the
# module-name allowlist). Header-only, so it is an INTERFACE import keyed on
# its include dir rather than a library.
if(DEFINED LOGOS_CONTAINER_ROOT AND EXISTS "${LOGOS_CONTAINER_ROOT}/include")
  message(STATUS "Using logos-container at ${LOGOS_CONTAINER_ROOT}")

  add_library(logos_container INTERFACE IMPORTED GLOBAL)
  set_target_properties(logos_container PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_CONTAINER_ROOT}/include"
    INTERFACE_LINK_LIBRARIES "nlohmann_json::nlohmann_json"
  )
else()
  message(FATAL_ERROR "LOGOS_CONTAINER_ROOT not set or ${LOGOS_CONTAINER_ROOT}/include does not exist")
endif()

# Find logos-container-subprocess (the process-isolation ModuleContainer).
# A single Qt-free static lib (the parent-side container) linked into
# logos_core. There is no child-side library: the child just reads its token
# from stdin, handled by logos_host's own TokenSource.
if(DEFINED LOGOS_CONTAINER_SUBPROCESS_ROOT AND EXISTS "${LOGOS_CONTAINER_SUBPROCESS_ROOT}/lib")
  message(STATUS "Using logos-container-subprocess at ${LOGOS_CONTAINER_SUBPROCESS_ROOT}")

  find_library(LOGOS_CONTAINER_SUBPROCESS_LIBRARY
    NAMES logos_container_subprocess
    PATHS "${LOGOS_CONTAINER_SUBPROCESS_ROOT}/lib"
    NO_DEFAULT_PATH
  )

  if(NOT LOGOS_CONTAINER_SUBPROCESS_LIBRARY)
    message(FATAL_ERROR "logos_container_subprocess library not found in ${LOGOS_CONTAINER_SUBPROCESS_ROOT}/lib")
  endif()

  # Declare the dependency on the header-only contract and on spdlog/Boost so
  # CMake orders the link correctly for GNU ld and pulls them in.
  add_library(logos_container_subprocess STATIC IMPORTED GLOBAL)
  set_target_properties(logos_container_subprocess PROPERTIES
    IMPORTED_LOCATION "${LOGOS_CONTAINER_SUBPROCESS_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_CONTAINER_SUBPROCESS_ROOT}/include"
    INTERFACE_LINK_LIBRARIES "logos_container;Boost::process;spdlog::spdlog"
  )
else()
  message(FATAL_ERROR "LOGOS_CONTAINER_SUBPROCESS_ROOT not set or ${LOGOS_CONTAINER_SUBPROCESS_ROOT}/lib does not exist")
endif()

# Find logos-module-loader (the header-only ModuleFormatLoader contract).
if(DEFINED LOGOS_MODULE_LOADER_ROOT AND EXISTS "${LOGOS_MODULE_LOADER_ROOT}/include")
  message(STATUS "Using logos-module-loader at ${LOGOS_MODULE_LOADER_ROOT}")

  add_library(logos_module_loader INTERFACE IMPORTED GLOBAL)
  set_target_properties(logos_module_loader PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_MODULE_LOADER_ROOT}/include"
    INTERFACE_LINK_LIBRARIES "logos_container"
  )
else()
  message(FATAL_ERROR "LOGOS_MODULE_LOADER_ROOT not set or ${LOGOS_MODULE_LOADER_ROOT}/include does not exist")
endif()

# Find logos-module-loader-qt (the QtPluginFormatLoader parent-side lib). The
# logos_host_qt binary also lives in that package; liblogos only links the lib
# (into logos_core) and re-exports the binary via bin.nix.
if(DEFINED LOGOS_MODULE_LOADER_QT_ROOT AND EXISTS "${LOGOS_MODULE_LOADER_QT_ROOT}/lib")
  message(STATUS "Using logos-module-loader-qt at ${LOGOS_MODULE_LOADER_QT_ROOT}")

  find_library(LOGOS_MODULE_LOADER_QT_LIBRARY
    NAMES logos_module_loader_qt
    PATHS "${LOGOS_MODULE_LOADER_QT_ROOT}/lib"
    NO_DEFAULT_PATH
  )

  if(NOT LOGOS_MODULE_LOADER_QT_LIBRARY)
    message(FATAL_ERROR "logos_module_loader_qt library not found in ${LOGOS_MODULE_LOADER_QT_ROOT}/lib")
  endif()

  add_library(logos_module_loader_qt STATIC IMPORTED GLOBAL)
  set_target_properties(logos_module_loader_qt PROPERTIES
    IMPORTED_LOCATION "${LOGOS_MODULE_LOADER_QT_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_MODULE_LOADER_QT_ROOT}/include"
    INTERFACE_LINK_LIBRARIES "logos_module_loader;Boost::filesystem;spdlog::spdlog"
  )
else()
  message(FATAL_ERROR "LOGOS_MODULE_LOADER_QT_ROOT not set or ${LOGOS_MODULE_LOADER_QT_ROOT}/lib does not exist")
endif()


# Define the library sources.
# The container contract (ModuleContainer + descriptor value types) lives in
# logos-container, the subprocess container in logos-container-subprocess, the
# ModuleFormatLoader contract in logos-module-loader, and the Qt-plugin loader +
# host binary in logos-module-loader-qt — all linked as imported targets below.
# The ModuleLoader base, the composite/registry orchestration, and the logging
# foundation are core concerns and stay here.
set(LOGOS_CORE_SOURCES
    logging/logos_log.cpp
    logging/logos_log.h
    logos_core/logos_core.cpp
    logos_core/logos_core.h
    logos_core/module_registry.cpp
    logos_core/module_registry.h
    logos_core/dependency_resolver.cpp
    logos_core/dependency_resolver.h
    logos_core/access_policy.cpp
    logos_core/access_policy.h
    logos_core/module_manager.cpp
    logos_core/module_manager.h
    logos_core/module_loader.h
    logos_core/composite_module_loader.cpp
    logos_core/composite_module_loader.h
    logos_core/module_loader_registry.cpp
    logos_core/module_loader_registry.h
)

# The Qt-plugin format loader (QtPluginFormatLoader) and the logos_host_qt host
# binary now live in logos-module-loader-qt; the ModuleFormatLoader interface
# lives in logos-module-loader. liblogos links the loader lib (imported below)
# and re-exports the host binary from that package via bin.nix.

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
find_package(Boost REQUIRED COMPONENTS process filesystem)

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

# Link Qt libraries, SDK, module library, and package-manager library.
# logos_container_subprocess (the parent-side ModuleContainer) and
# logos_container (the contract + logging foundation it and logos_core share)
# are linked here; Boost::process/spdlog satisfy their transitive symbols.
target_link_libraries(logos_core PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
    logos_sdk
    logos_module
    process_stats
    logos_container
    logos_container_subprocess
    logos_module_loader_qt
    package_manager_lib
    nlohmann_json::nlohmann_json
    Boost::process
    spdlog::spdlog
)

# Include directories for the library
target_include_directories(logos_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/logos_core
    ${CMAKE_CURRENT_SOURCE_DIR}/module_loaders
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
