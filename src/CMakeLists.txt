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

# Define module_lib sources (Qt plugin abstraction layer)
set(MODULE_LIB_SOURCES
    module_lib/module_metadata.cpp
    module_lib/module_metadata.h
    module_lib/module_loader.cpp
    module_lib/module_loader.h
    module_lib/module_introspection.cpp
    module_lib/module_introspection.h
    module_lib/module_lib.h
)

# Define the library sources
set(LOGOS_CORE_SOURCES
    logos_core/logos_core.cpp
    logos_core/logos_core.h
    logos_core/logos_core_internal.h
    logos_core/core_context.cpp
    logos_core/app_lifecycle.cpp
    logos_core/app_lifecycle.h
    logos_core/plugin_manager.cpp
    logos_core/plugin_manager.h
    logos_core/proxy_api.cpp
    logos_core/proxy_api.h
    logos_core/process_stats.cpp
    logos_core/process_stats.h
    logos_core/core_manager/core_manager.cpp
    logos_core/core_manager/core_manager.h
    logos_core/core_manager/core_manager_interface.h
    common/interface.h
    ${MODULE_LIB_SOURCES}
)

# Define the host application sources
set(HOST_SOURCES
    ${CMAKE_SOURCE_DIR}/app/main.cpp
    ${CMAKE_SOURCE_DIR}/app/command_line_parser.cpp
    ${CMAKE_SOURCE_DIR}/app/call_executor.cpp
)

# Define the logos host sources
set(LOGOS_HOST_SOURCES
    logos_host/logos_host.cpp
    logos_host/command_line_parser.cpp
    logos_host/command_line_parser.h
    logos_host/plugin_initializer.cpp
    logos_host/plugin_initializer.h
    common/interface.h
    ${MODULE_LIB_SOURCES}
)

# Create the logos core library
# Build as static for iOS (dynamic libraries not supported in iOS apps), shared otherwise
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_library(logos_core STATIC ${LOGOS_CORE_SOURCES})
else()
    add_library(logos_core SHARED ${LOGOS_CORE_SOURCES})
endif()

# Set the LOGOS_CORE_LIBRARY definition for the library
target_compile_definitions(logos_core PRIVATE LOGOS_CORE_LIBRARY)

# Link Qt libraries and SDK to the library
target_link_libraries(logos_core PUBLIC 
    Qt${QT_VERSION_MAJOR}::Core 
    Qt${QT_VERSION_MAJOR}::RemoteObjects
    logos_sdk
)

# Include directories for the library
target_include_directories(logos_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/logos_core
    ${CMAKE_CURRENT_SOURCE_DIR}/module_lib
    ${Qt${QT_VERSION_MAJOR}_INCLUDE_DIRS}
)

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

# Skip RPATH settings and executables for iOS (not applicable)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
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

    # Create the host application
    add_executable(logoscore ${HOST_SOURCES})

    # Link the host application with the logos core library
    target_link_libraries(logoscore PRIVATE logos_core)

    # Include directories for the host application
    target_include_directories(logoscore PRIVATE
        ${CMAKE_SOURCE_DIR}/app
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/logos_core
    )

    # Create the logos host application
    add_executable(logos_host ${LOGOS_HOST_SOURCES})

    # Link the logos host with Qt libraries
    target_link_libraries(logos_host PRIVATE 
        Qt${QT_VERSION_MAJOR}::Core 
        Qt${QT_VERSION_MAJOR}::RemoteObjects
        logos_sdk
    )

    # Include directories for the logos host application
    target_include_directories(logos_host PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/logos_host
        ${CMAKE_CURRENT_SOURCE_DIR}/module_lib
        ${Qt${QT_VERSION_MAJOR}_INCLUDE_DIRS}
    )

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

        set_target_properties(logoscore PROPERTIES
            INSTALL_RPATH "${_qt_exec_runtime_paths}"
            BUILD_WITH_INSTALL_RPATH TRUE)
    else()
        set_target_properties(logoscore PROPERTIES
            INSTALL_RPATH "$ORIGIN/../lib"
            BUILD_WITH_INSTALL_RPATH TRUE)
    endif()

    # Set RPATH settings for the logos host executable
    if(APPLE)
        set(_qt_host_runtime_paths "@loader_path/../lib")
        if(DEFINED Qt${QT_VERSION_MAJOR}_DIR)
            get_filename_component(_qt_host_lib_dir "${Qt${QT_VERSION_MAJOR}_DIR}/../.." REALPATH)
            list(APPEND _qt_host_runtime_paths "${_qt_host_lib_dir}")
        endif()
        list(REMOVE_DUPLICATES _qt_host_runtime_paths)

        set_target_properties(logos_host PROPERTIES
            INSTALL_RPATH "${_qt_host_runtime_paths}"
            BUILD_WITH_INSTALL_RPATH TRUE)
    else()
        set_target_properties(logos_host PROPERTIES
            INSTALL_RPATH "$ORIGIN/../lib"
            BUILD_WITH_INSTALL_RPATH TRUE)
    endif()
endif() 
