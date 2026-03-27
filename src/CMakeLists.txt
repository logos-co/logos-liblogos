set(CMAKE_AUTOMOC ON)

set(_default_sdk_root "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/logos-cpp-sdk")
if(NOT DEFINED LOGOS_CPP_SDK_ROOT OR LOGOS_CPP_SDK_ROOT STREQUAL "")
  set(LOGOS_CPP_SDK_ROOT "${_default_sdk_root}")
endif()

get_filename_component(LOGOS_CPP_SDK_ROOT "${LOGOS_CPP_SDK_ROOT}" ABSOLUTE)

# Check if we have a built SDK package (with lib and include directories)
if(EXISTS "${LOGOS_CPP_SDK_ROOT}/lib" AND EXISTS "${LOGOS_CPP_SDK_ROOT}/include")
  # Use the built SDK package
  message(STATUS "Using built logos-cpp-sdk at ${LOGOS_CPP_SDK_ROOT}")
  
  # Find the logos_sdk library
  find_library(LOGOS_SDK_LIBRARY
    NAMES logos_sdk
    PATHS "${LOGOS_CPP_SDK_ROOT}/lib"
    NO_DEFAULT_PATH
  )
  
  if(NOT LOGOS_SDK_LIBRARY)
    message(FATAL_ERROR "logos_sdk library not found in ${LOGOS_CPP_SDK_ROOT}/lib")
  endif()
  
  # Create an imported target for the SDK
  add_library(logos_sdk STATIC IMPORTED)
  set_target_properties(logos_sdk PROPERTIES
    IMPORTED_LOCATION "${LOGOS_SDK_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_CPP_SDK_ROOT}/include"
  )
  
elseif(EXISTS "${LOGOS_CPP_SDK_ROOT}/cpp/CMakeLists.txt")
  # Use the source SDK (original behavior)
  message(STATUS "Using logos-cpp-sdk source at ${LOGOS_CPP_SDK_ROOT}")
  add_subdirectory(${LOGOS_CPP_SDK_ROOT}/cpp ${CMAKE_BINARY_DIR}/sdk)
else()
  message(FATAL_ERROR "logos-cpp-sdk not found at ${LOGOS_CPP_SDK_ROOT}")
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
    logos_core/app_lifecycle.cpp
    logos_core/app_lifecycle.h
    logos_core/plugin_registry.cpp
    logos_core/plugin_registry.h
    logos_core/dependency_resolver.cpp
    logos_core/dependency_resolver.h
    logos_core/plugin_loader.cpp
    logos_core/plugin_loader.h
    logos_core/plugin_manager.cpp
    logos_core/plugin_manager.h
    logos_core/qt/qt_app_context.cpp
    logos_core/qt/qt_app_context.h
    logos_core/qt/qt_process_manager.cpp
    logos_core/qt/qt_process_manager.h
)

# Define the logos host sources
set(LOGOS_HOST_SOURCES
    logos_host/logos_host.cpp
    logos_host/command_line_parser.cpp
    logos_host/command_line_parser.h
    logos_host/plugin_initializer.cpp
    logos_host/plugin_initializer.h
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
)

# Include directories for the library
target_include_directories(logos_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/logos_core
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
add_executable(logos_host ${LOGOS_HOST_SOURCES})

# Link the logos host with Qt libraries and module library
target_link_libraries(logos_host PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
    logos_sdk
    logos_module
)

# Include directories for the logos host application
target_include_directories(logos_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/logos_host
    ${Qt${QT_VERSION_MAJOR}_INCLUDE_DIRS}
)

# Add logos_module include directory if available
if(DEFINED LOGOS_MODULE_ROOT AND EXISTS "${LOGOS_MODULE_ROOT}/include")
    target_include_directories(logos_host PRIVATE
        ${LOGOS_MODULE_ROOT}/include
        ${LOGOS_MODULE_ROOT}/include/module_lib
    )
endif()

# Add SDK include directory if using built SDK
if(EXISTS "${LOGOS_CPP_SDK_ROOT}/lib" AND EXISTS "${LOGOS_CPP_SDK_ROOT}/include")
    target_include_directories(logos_host PRIVATE
        ${LOGOS_CPP_SDK_ROOT}/include
        ${LOGOS_CPP_SDK_ROOT}/include/cpp
        ${LOGOS_CPP_SDK_ROOT}/include/core
    )
else()
    target_include_directories(logos_host PRIVATE ${LOGOS_CPP_SDK_ROOT}/cpp)
endif()

# Set RPATH settings for the executable
if(APPLE)
    set(_qt_exec_runtime_paths "@loader_path/../lib")
    if(DEFINED Qt${QT_VERSION_MAJOR}_DIR)
        get_filename_component(_qt_exec_lib_dir "${Qt${QT_VERSION_MAJOR}_DIR}/../.." REALPATH)
        list(APPEND _qt_exec_runtime_paths "${_qt_exec_lib_dir}")
    endif()
    list(REMOVE_DUPLICATES _qt_exec_runtime_paths)

    set_target_properties(logos_host PROPERTIES
        INSTALL_RPATH "${_qt_exec_runtime_paths}"
        BUILD_WITH_INSTALL_RPATH TRUE)
else()
    set_target_properties(logos_host PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_WITH_INSTALL_RPATH TRUE)
endif()
