# liblogos is a Qt-free parent process. It links only the plain protocol C ABI;
# logos_host_qt remains available as the compatibility child for Qt plugins.
if(NOT DEFINED LOGOS_PROTOCOL_ROOT AND DEFINED ENV{LOGOS_PROTOCOL_ROOT})
  set(LOGOS_PROTOCOL_ROOT "$ENV{LOGOS_PROTOCOL_ROOT}")
endif()
if(EXISTS "${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol")
  message(STATUS "Using logos-protocol at ${LOGOS_PROTOCOL_ROOT}")
  find_package(logos-protocol REQUIRED
    PATHS "${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol" NO_DEFAULT_PATH)
else()
  message(FATAL_ERROR "logos-protocol not found. Set LOGOS_PROTOCOL_ROOT to the plain package.")
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

# Container implementation — selected by the BUILD, not named here.
#
# The chosen implementation package ships a generic CMake config package
# (LogosContainerImpl) that defines the imported target LogosContainerImpl::impl,
# carrying its static library AND its own transitive deps (Boost, spdlog, ...).
# liblogos just asks for "the container implementation" by that generic name and
# links the target — it knows neither the library name nor the dependencies. nix
# puts the chosen package on CMAKE_PREFIX_PATH (default logos-container-subprocess);
# switching to another container (Docker, ...) means a package that provides the
# same config — no edit to this file, the nix flags, or any C++.
find_package(LogosContainerImpl REQUIRED)

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

# Format-loader implementation — selected by the BUILD (mirrors the container).
# The chosen package ships a generic CMake config package (LogosFormatLoaderImpl)
# defining the imported target LogosFormatLoaderImpl::impl with its own deps.
# Default: logos-module-loader-qt (which also ships the logos_host_qt binary,
# re-exported via bin.nix — independent of this lib link).
find_package(LogosFormatLoaderImpl REQUIRED)


# Define the library sources.
# The container contract (ModuleContainer + descriptor value types) lives in
# logos-container and the ModuleFormatLoader contract in logos-module-loader.
# The concrete container + format-loader implementations are NOT named here:
# they are linked through the build-selected `LogosContainerImpl::impl` /
# `LogosFormatLoaderImpl::impl` targets found via find_package above (default
# subprocess + qt, chosen in nix/default.nix). The ModuleLoader base, the
# composite/registry orchestration, and the logging foundation are core concerns
# and stay here.
set(LOGOS_CORE_SOURCES
    logging/logos_log.cpp
    logging/logos_log.h
    logos_core/logos_core.cpp
    logos_core/logos_core.h
    logos_core/module_registry.cpp
    logos_core/module_registry.h
    logos_core/dependency_resolver.cpp
    logos_core/dependency_resolver.h
    logos_core/dependency_gate.cpp
    logos_core/dependency_gate.h
    logos_core/access_policy.cpp
    logos_core/access_policy.h
    logos_core/bootstrap_policy.cpp
    logos_core/bootstrap_policy.h
    logos_core/inproc_module_loader.cpp
    logos_core/inproc_module_loader.h
    logos_core/token_authority.cpp
    logos_core/token_authority.h
    logos_core/package_config.cpp
    logos_core/package_config.h
    core_service/call_envelope.cpp
    core_service/call_envelope.h
    core_service/embedded_core_service.cpp
    core_service/embedded_core_service.h
    core_service/shell_binding.cpp
    core_service/shell_binding.h
    runtime_host/runtime_host.cpp
    runtime_host/runtime_host.h
    runtime_host/runtime_spawn.cpp
    logos_core/instance_id.h
    logos_core/module_state_observer.cpp
    logos_core/module_state_observer.h
    logos_core/module_manager.cpp
    logos_core/module_manager.h
    logos_core/module_loader.h
    logos_core/composite_module_loader.cpp
    logos_core/composite_module_loader.h
    logos_core/module_loader_registry.cpp
    logos_core/module_loader_registry.h
)

# The format-loader implementation and the logos_host_qt host binary live in the
# package the build selects (default logos-module-loader-qt); the
# ModuleFormatLoader interface lives in logos-module-loader. liblogos links the
# implementation lib via the find_package'd `LogosFormatLoaderImpl::impl` target
# and re-exports the host binary from the same package via bin.nix.

# Create the logos core library
add_library(logos_core SHARED ${LOGOS_CORE_SOURCES})

# Set the LOGOS_CORE_LIBRARY definition for the library
target_compile_definitions(logos_core PRIVATE LOGOS_CORE_LIBRARY)

# PE export table: nothing to do here. LOGOS_CORE_EXPORT now expands to
# __declspec(dllexport) on Windows (see logos_core/logos_core.h), which both
# exports the C API and, because GNU ld disables auto-export image-wide once any
# symbol is dllexported, stops the internal C++ surface leaking.
#
# This deliberately REPLACES an earlier `-Wl,--export-all-symbols`. That did
# export the C API, but it also exported all 13,252 symbols in the image --
# LogosAPI's among them -- so any consumer linking both liblogos_core and the
# qt-sdk static library failed with "multiple definition of `LogosAPI::LogosAPI'".
# Do not reinstate it; if the C API ever stops being exported, the cause is a
# missing LOGOS_CORE_EXPORT on a declaration, not a missing linker flag.
#
# liblogos_core links the Qt-free protocol runtime directly. Qt plugin loading
# and the legacy Qt SDK live in logos_host_qt, a separate child executable, so
# no shared Qt/SDK singleton needs to be exported from the parent library.

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
  # On Windows a SHARED IMPORTED target links through its IMPORT library, and
  # CMake hard-errors at generate time when IMPORTED_IMPLIB is unset. find_library
  # returns libpackage_manager_lib.dll.a there (CMAKE_FIND_LIBRARY_SUFFIXES for
  # MinGW is .dll.a;.a), which is exactly the import library to link.
  if(WIN32)
    set_target_properties(package_manager_lib PROPERTIES
      IMPORTED_IMPLIB "${LOGOS_PACKAGE_MANAGER_LIBRARY}"
    )
  endif()

  # liblgx, for the semver the dependency gate evaluates ranges with. It ships
  # in the same root (lgx.h is already on package_manager_lib's include dirs)
  # but package_manager_lib only IMPORTS it, so it must be named here too.
  find_library(LOGOS_LGX_LIBRARY
    NAMES lgx
    PATHS "${LOGOS_PACKAGE_MANAGER_ROOT}/lib"
    NO_DEFAULT_PATH
  )

  if(NOT LOGOS_LGX_LIBRARY)
    message(FATAL_ERROR "lgx not found in ${LOGOS_PACKAGE_MANAGER_ROOT}/lib")
  endif()

  add_library(lgx_lib SHARED IMPORTED)
  set_target_properties(lgx_lib PROPERTIES
    IMPORTED_LOCATION "${LOGOS_LGX_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_PACKAGE_MANAGER_ROOT}/include"
  )
  if(WIN32)
    set_target_properties(lgx_lib PROPERTIES
      IMPORTED_IMPLIB "${LOGOS_LGX_LIBRARY}"
    )
  endif()
else()
  message(FATAL_ERROR "LOGOS_PACKAGE_MANAGER_ROOT not set or ${LOGOS_PACKAGE_MANAGER_ROOT}/lib does not exist")
endif()

# Link the Qt-free parent runtime and package-manager library.
# The container/format-loader implementations are linked through the generic,
# build-selected `LogosContainerImpl::impl` / `LogosFormatLoaderImpl::impl`
# targets (each comes from a find_package config that carries its own deps);
# `logos_container` is the shared contract. liblogos names no specific
# implementation here.
target_link_libraries(logos_core PUBLIC
    logos-protocol::logos_protocol_plain_shared
    process_stats
    logos_container
    LogosContainerImpl::impl
    LogosFormatLoaderImpl::impl
    package_manager_lib
    lgx_lib
    nlohmann_json::nlohmann_json
    Boost::process
    spdlog::spdlog
)

# The native host's module bring-up, for modules placed in-process. It links no
# lp_* runtime: these modules run on the shared one above.
if(NOT TARGET LogosFormatLoaderImpl::native_module_host)
    message(FATAL_ERROR "The format loader provides no native_module_host; "
                        "logos-module-loader-qt with logos_native_module_host is required")
endif()
target_link_libraries(logos_core PRIVATE LogosFormatLoaderImpl::native_module_host)
# Its lp_* calls resolve against that shared runtime. A PE import library links
# like an archive, in order, so the runtime has to follow the host on the line.
set_property(TARGET LogosFormatLoaderImpl::native_module_host APPEND PROPERTY
             INTERFACE_LINK_LIBRARIES logos-protocol::logos_protocol_plain_shared)

# capability_module's engine interface (logos_capability_engine.h), from its source.
if(NOT DEFINED LOGOS_CAPABILITY_ENGINE_INCLUDE AND DEFINED ENV{LOGOS_CAPABILITY_ENGINE_INCLUDE})
  set(LOGOS_CAPABILITY_ENGINE_INCLUDE "$ENV{LOGOS_CAPABILITY_ENGINE_INCLUDE}")
endif()
if(NOT EXISTS "${LOGOS_CAPABILITY_ENGINE_INCLUDE}/logos_capability_engine.h")
  message(FATAL_ERROR "logos_capability_engine.h not found; set LOGOS_CAPABILITY_ENGINE_INCLUDE")
endif()
target_include_directories(logos_core PRIVATE "${LOGOS_CAPABILITY_ENGINE_INCLUDE}")

# Include directories for the library
target_include_directories(logos_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/logos_core
    ${CMAKE_CURRENT_SOURCE_DIR}/module_loaders
)

# Add process-stats include directory if available
if(DEFINED PROCESS_STATS_ROOT AND EXISTS "${PROCESS_STATS_ROOT}/include")
  target_include_directories(logos_core PRIVATE ${PROCESS_STATS_ROOT}/include)
endif()


# Set RPATH settings for the library
if(APPLE)
    set_target_properties(logos_core PROPERTIES
        INSTALL_RPATH "@loader_path"
        INSTALL_NAME_DIR "@rpath"
        BUILD_WITH_INSTALL_NAME_DIR TRUE
        BUILD_WITH_INSTALL_RPATH TRUE)
else()
    set_target_properties(logos_core PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH TRUE)
endif()

# The runtime in a process of its own, which an app spawns (logos_runtime_spawn).
add_executable(logos_runtime runtime_host/main.cpp)
target_link_libraries(logos_runtime PRIVATE logos_core)
target_include_directories(logos_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/logos_core)
if(APPLE)
    set_target_properties(logos_runtime PROPERTIES
        INSTALL_RPATH "@loader_path/../lib"
        BUILD_WITH_INSTALL_RPATH TRUE)
elseif(NOT WIN32)
    set_target_properties(logos_runtime PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_WITH_INSTALL_RPATH TRUE)
endif()
install(TARGETS logos_runtime RUNTIME DESTINATION bin)

# The test suite drives the internal C++ classes as well as the C API, and on
# Windows the DLL exports only the C API (see above). There the suite links the
# same sources as a static library, compiled and linked as logos_core is.
if(WIN32 AND LOGOS_BUILD_TESTS)
    add_library(logos_core_static STATIC ${LOGOS_CORE_SOURCES})
    get_target_property(_logos_core_includes logos_core INCLUDE_DIRECTORIES)
    get_target_property(_logos_core_definitions logos_core COMPILE_DEFINITIONS)
    get_target_property(_logos_core_links logos_core LINK_LIBRARIES)
    target_include_directories(logos_core_static PRIVATE ${_logos_core_includes})
    target_compile_definitions(logos_core_static PRIVATE ${_logos_core_definitions})
    # PUBLIC, so the tests declare the C API as the library's own, not dllimport.
    target_compile_definitions(logos_core_static PUBLIC LOGOS_CORE_LIBRARY)
    target_link_libraries(logos_core_static PUBLIC ${_logos_core_links})
endif()
