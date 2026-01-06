note: WIP, see https://github.com/logos-co/logos-core-poc/blob/develop/docs/specs.md for an overall documentation.

# Logos Core Library

## Table of Contents

- [1. Overview and Goals](#1-overview-and-goals)
- [2. Architecture](#2-architecture)
  - [2.1 High-level Structure](#21-high-level-structure)
  - [2.2 Process Separation](#22-process-separation)
  - [2.3 Core Manager](#23-core-manager)
  - [2.4 Module Host](#24-module-host)
  - [2.5 Local Mode (In-Process)](#25-local-mode-in-process)
  - [2.6 Tokens and Authentication](#26-tokens-and-authentication)
- [3. API Description](#3-api-description)
  - [3.1 Core API Functions](#31-core-api-functions)
    - [Mode and Initialization](#mode-and-initialization)
    - [Plugin Management](#plugin-management)
    - [Local and Static Plugin Support](#local-and-static-plugin-support)
    - [Token and Monitoring](#token-and-monitoring)
    - [Async Operations](#async-operations)
  - [3.2 Core Manager Module](#32-core-manager-module)
  - [3.3 Capability Module](#33-capability-module)
  - [3.4 SDK Overview](#34-sdk-overview)
    - [LogosAPI](#logosapi)
    - [LogosAPIProvider](#logosapiprovider)
    - [LogosAPIClient](#logosapiclient)
    - [Generated C++ Wrappers](#generated-c-wrappers)
- [4. Implementation](#4-implementation)
  - [4.1 Library Structure](#41-library-structure)
  - [4.2 Build System](#42-build-system)
  - [4.3 Implementation Details](#43-implementation-details)
  - [4.4 Plugin Interface and Metadata](#44-plugin-interface-and-metadata)
- [5. Usage](#5-usage)
  - [5.1 Basic Usage](#51-basic-usage)
  - [5.2 Module Lifecycle](#52-module-lifecycle)
  - [5.3 Sequence of Operations](#53-sequence-of-operations)
  - [5.4 Local Mode Usage](#54-local-mode-usage)

## 1. Overview and Goals

Logos Core is a modular platform designed to host and interact with independently developed modules (plugins). The core library (`logos-liblogos`) and accompanying C++ SDK ([logos-cpp-sdk/cpp](https://github.com/logos-co/logos-cpp-sdk)) provide a modular, plug-in-based runtime for decentralised applications. Each module implements a common interface and is launched in its own process for isolation. The C++ SDK exposes a remote-procedure-call (RPC) mechanism so that modules, the core and external modules can call methods on each other or listen for events.

The core exposes an extensible API to load, start, stop and introspect plug-ins, and it wraps Qt Remote Objects to allow modules to call each other's methods asynchronously. The SDK supplies client and provider classes that abstract away remote-object registry and token management, enabling modules to perform RPC-like calls without needing to understand the underlying IPC mechanism. The core also supports a `local` mode meant for mobile platforms in which is not possible to run modules in different processes.

This documentation refers to some components that are implemented in the [C++ SDK](https://github.com/logos-co/logos-cpp-sdk) and [Capability Module](https://github.com/logos-co/logos-capability-module) as they are critical components to understand how Logos Core works.

### Main Repository Components

| Component | Purpose |
|-----------|---------|
| `src/logos_core.cpp` | liblogos library |
| `src/logos_host.cpp` | module host that loads a single module in its own process |
| `src/core_manager/` (built-in module) | Module that exposes core functionality as a module |
| `interface.h` | Plugin interface definition that all modules must implement |

### Other Repository Components

| Component | Purpose |
|-----------|---------|
| `examples/` | Example cli app to run logos core |
| `nix/` | Nix scripts |
| `scripts/` | compilation scripts for iOS |

### Components

| Component | Purpose |
|-----------|---------|
| `liblogos_core` (shared library) | Core library with C API for module management |
| `logoscore` (executable) | Standalone core executable |
| `logos_host` (executable) | Lightweight executable that loads a single module in its own process |
| `core_manager` (built-in module) | Module that exposes core functionality as a module |
| `interface.h` | Plugin interface definition that all modules must implement |

## 2. Architecture

### 2.1 High-level Structure

At a high level, the Logos Core consists of:

**Core Library** – The C/C++ library (`liblogos_core`) that provides the core API functions for module management.

**Core Manager** – A built-in module that runs in the core process and exposes core functionality via RPC, allowing modules to manage the core without linking against the C API.

**Module Host** – A lightweight executable (`logos_host`) that loads a single module in its own process. It communicates with the core over a local socket to receive an authentication token and registers the module's object with the remote registry.

Abstracted in the [C++ SDK](https://github.com/logos-co/logos-cpp-sdk):

**SDK/CPP LogosAPI** – A client library used by modules and external applications to call methods on remote modules and to listen for events. It encapsulates connection management, token handling and asynchronous invocation. Key classes include `LogosAPI`, `LogosAPIProvider`, `LogosAPIClient`, `LogosAPIConsumer`, `ModuleProxy` and `TokenManager`.

**Remote Object Registry** – A registry that maintains a mapping of module names to remote object replicas and forwards method calls/events. In the PoC it is implemented using `QRemoteObjectRegistryHost` and `QRemoteObjectHost`. The core and each module maintain their own registry in which an object is exposed.

### 2.2 Process Separation and IPC

Each module runs in its own process to improve robustness and security. The core spawns a `logos_host` process per module and communicates via a local inter-process socket. After a module is authenticated, both processes use the remote-object registry to call methods or deliver events. This separation isolates faulty or untrusted modules and allows modules to be written in different languages as long as they implement the agreed RPC protocol.

In **Local mode** (see [2.5](#25-local-mode-in-process)) module objects stay in-process and communicate via the SDK's PluginRegistry instead of spawning `logos_host` processes.

### 2.3 Tokens and Authentication

Since `QRemoteObjectRegistryHost` has no built-in security mechanisms, by default it's not possible to know the origin of a request and if they are authorized. For this reason internally all remote calls require an authentication token, however this is done under the hood through the API and invisible for the developer when they are using the `LogosAPI` in their modules. When a module is loaded, the core generates a token and sends it to the module process. This way the module can authenticate calls that are coming from the core.
Then when modules need to communicate with each other, they request authorization from the Capability Module, which issues a token and notifies both. The modules then use this token for subsequent requests.
Each Module stores tokens in a thread-safe `TokenManager`, this is done internally and is transparent for the Developer.`ModuleProxy` validates tokens before dispatching method calls to the underlying module implementation.

### 2.4 Local Mode (In-Process)

Local mode is used on platforms where spawning extra processes or using dynamic plugins is constrained (e.g., iOS, mobile/embedded apps):

- Call `logos_core_set_mode(LOGOS_MODE_LOCAL)` before `logos_core_start()` to bypass `logos_host`.
- Modules are hosted in-process via `QPluginLoader::staticInstances()` or by registering existing QObject plugin instances; RPC is routed through the SDK's PluginRegistry rather than Qt Remote Objects.
- Authentication tokens are still generated and shared so modules can talk to core, core_manager, and capability services.
- Static-plugin helpers make it possible to bundle plugins with the app via `Q_IMPORT_PLUGIN` and register them at runtime.

## 3. API Description

### 3.1 Core API Functions

The core library provides a C API defined in `logos_core.h`:

#### Mode and Initialization

| Function | Purpose |
|----------|---------|
| `logos_core_init(argc, argv)` | Initializes global state and optionally sets the plugin directory. Creates a QCoreApplication if one does not exist. |
| `logos_core_set_mode(mode)` | Switches between `LOGOS_MODE_REMOTE` (default, process-isolated via `logos_host`) and `LOGOS_MODE_LOCAL` (in-process, for mobile/embedded). Must be called before `logos_core_start()`. |
| `logos_core_set_plugins_dir(path)` | Specifies where to look for modules. Must be called before starting. |
| `logos_core_start()` | Scans the plugin directory, processes metadata, creates the Core Manager module, loads the built-in capability module, and starts the remote object registry. |
| `logos_core_exec()` | Runs the Qt event loop. Returns when the application exits. |
| `logos_core_cleanup()` | Unloads all modules, stops processes and cleans up global state. |
| `logos_core_get_module_stats() → char*` | Returns a compact JSON array of CPU percentage, CPU time, and memory usage for each loaded plugin process (excludes `core_manager`). Not available on iOS. Caller must free the returned string. |

#### Plugin Management

| Function | Purpose |
|----------|---------|
| `logos_core_get_loaded_plugins() → char**` | Returns a null-terminated array of currently loaded module names. Caller must free the array. |
| `logos_core_get_known_plugins() → char**` | Returns a null-terminated array of all discovered modules (even if not loaded). Caller must free the array. |
| `logos_core_load_plugin(name) → int` | Loads a module by name. In Remote mode it starts a `logos_host` process, sends an auth token, waits for registration. Returns 1 on success, 0 on failure. |
| `logos_core_unload_plugin(name) → int` | Terminates the module's process and removes it from loaded modules. Returns 1 on success, 0 on failure. |
| `logos_core_process_plugin(path) → char*` | Reads a module file's metadata and adds it to the list of known modules without loading it. Returns the module name or NULL on failure. Caller must free the returned string. |

#### Local and Static Plugin Support

| Function | Purpose |
|----------|---------|
| `logos_core_load_static_plugins() → int` | Local mode only. Loads statically linked plugins registered via `Q_IMPORT_PLUGIN` after `logos_core_start()`. Returns the number of plugins loaded. |
| `logos_core_register_plugin_instance(plugin_name, plugin_instance) → int` | Local mode only. Registers an already-created QObject instance that implements `PluginInterface`. Returns 1 on success. |
| `logos_core_register_plugin_by_name(plugin_name) → int` | Local mode only. Finds a statically linked plugin by name from `QPluginLoader::staticInstances()` and registers it. Returns 1 on success. |

#### Token and Monitoring

| Function | Purpose |
|----------|---------|
| `logos_core_get_token(key) → char*` | Returns the auth token associated with a key. Caller must free the returned string. Returns NULL if not found. |

#### Async Operations

These are experimental APIs and currently being used by the examples that use NodeJS/Electron. Since apps using NodeJS do not have QT and therefore cannot easily use the QT Remote API directly, they can instead make the calls through the Core. Note that besides this initial call, all other calls between modules still happen directly between modules using Qt Remote and do not go through the core.

| Function | Purpose |
|----------|---------|
| `logos_core_async_operation(data, callback, user_data)` | Example async helper that returns a success message after a short delay (useful for bindings/language tests). |
| `logos_core_load_plugin_async(plugin_name, callback, user_data)` | Asynchronously loads a known plugin and reports success/failure via callback. |
| `logos_core_call_plugin_method_async(plugin_name, method_name, params_json, callback, user_data)` | Invokes a method on a loaded plugin asynchronously. `params_json` is an array of `{name, value, type}` objects; the callback receives success/failure details. |
| `logos_core_register_event_listener(plugin_name, event_name, callback, user_data)` | Registers an event listener on a loaded plugin. Triggers the callback with event JSON whenever the event is emitted. |
| `logos_core_process_events()` | Processes Qt events without blocking, allowing integration with external event loops. |

Async callbacks use the signature `typedef void (*AsyncCallback)(int result, const char* message, void* user_data);`.

### 3.2 Core Manager Module

The Core Manager is a built-in module that exposes core functionality via RPC. It implements `PluginInterface` and provides the following callable surface:

| Method | Purpose |
|--------|---------|
| `initialize(argc, argv)` | Prepare the module (currently a no-op). |
| `setPluginsDirectory(directory)` | Set the directory where the core should look for modules. |
| `start()` | Start the core's remote object registry and load built-in modules. |
| `cleanup()` | Unload all modules and shut down the core. |
| `getLoadedPlugins() → QStringList` | Return a list of names of currently loaded modules. |
| `getKnownPlugins() → QJsonArray` | Return a JSON array of all known modules with a `loaded` flag. |
| `loadPlugin(pluginName) → bool` | Load a plugin by name. Returns true on success. |
| `unloadPlugin(pluginName) → bool` | Unload a plugin by name. Returns true on success. |
| `processPlugin(filePath) → QString` | Read a plugin file's metadata and register it as known. Returns the module name or empty string on error. |
| `getPluginMethods(pluginName) → QJsonArray` | Introspect a module's methods using Qt meta-object introspection (used for debugging/introspection). |
| `helloWorld()` | Simple debug helper that logs a greeting for smoke testing RPC wiring. |
| `initLogos(LogosAPI*)` | Internal helper to stash a LogosAPI instance when embedded in Local mode flows. |

## 4. Implementation

### 4.1 Library Structure

The core library has the following structure:

```
logos-liblogos/
├── src/                          # Core library source
│   ├── logos_core.h/cpp         # C API implementation
│   ├── logos_host.cpp           # Module host executable
│   ├── main.cpp                  # Standalone core executable
│   └── core_manager/            # Core Manager module
│       ├── core_manager.h/cpp   # Core Manager implementation
│       └── core_manager_interface.h  # Core Manager interface
├── interface.h                   # Plugin interface definition
├── CMakeLists.txt                # CMake build configuration
├── nix/                          # Nix build configuration
│   ├── default.nix               # Common configuration
│   ├── build.nix                 # Shared build
│   ├── bin.nix                   # Binary build
│   ├── lib.nix                   # Library build
│   └── include.nix               # code generation build
└── docs/
    └── specs.md                  # This document
```

### 4.2 Build System

The core library supports two build systems: Nix (recommended) and CMake.

#### Nix Build System

The library includes a Nix flake for reproducible builds:

- `nix/default.nix`: Common configuration (dependencies, flags, metadata)
- `nix/build.nix`: Shared build that compiles everything once
- `nix/bin.nix`: Extracts binaries (includes libraries for runtime linking)
- `nix/lib.nix`: Extracts libraries only
- `nix/include.nix`: Header installation

**Build the library:**
```bash
nix build
```

This creates a `result` symlink with:
```
result/
├── bin/
│   ├── logoscore                 # Standalone core executable
│   └── logos_host                # Module host executable
├── lib/
│   ├── liblogos_core.dylib      # Core library
│   ├── liblogoscore.dylib        # Core executable library
│   └── liblogos_host.dylib      # Host executable library
└── include/
    └── interface.h               # Plugin interface
```

**Build individual components:**
```bash
# Build only the binaries
nix build '.#logos-liblogos-bin'

# Build only the libraries
nix build '.#logos-liblogos-lib'

# Build only the headers
nix build '.#logos-liblogos-include'
```

#### CMake Build Configuration

**Manual build:**
```bash
./scripts/compile.sh
```

The built libraries will be in `build/lib/` and binaries in `build/bin/`.

### 4.3 Implementation Details

#### Module Discovery

When `logos_core_start()` is called:

1. Core scans the plugins directory for `.so`, `.dylib`, or `.dll` files
2. For each file, calls `logos_core_process_plugin()` to read metadata
3. Metadata is parsed from the plugin's `metadata.json` (via `QPluginLoader`)
4. Modules are added to the "known" list
5. Built-in modules (capability_module) are automatically loaded

In Local mode, static or pre-instantiated modules can be registered after startup with `logos_core_load_static_plugins()`

#### Module Loading

In **Remote mode**, when `logos_core_load_plugin(name)` is called:

1. Core finds the plugin file for the given name
2. Core spawns a `logos_host` process with the plugin path
3. Core generates a UUID authentication token
4. Core sends the token to the host process via local socket
5. Host process loads the plugin and calls `initLogos(LogosAPI*)`
6. Host process registers the plugin with the remote object registry
7. Core waits for registration and records the module as loaded

In **Local mode** the plugin is loaded in-process through `QPluginLoader`, registered via the SDK PluginRegistry, and tokens are generated/stored without launching `logos_host`. Static plugins can also be registered via `logos_core_load_static_plugins()`, `logos_core_register_plugin_instance()`, or `logos_core_register_plugin_by_name()`.

### 4.4 Plugin Interface and Metadata

Modules must implement `PluginInterface` (in `interface.h`), expose `name()` and `version()`, and typically provide an `initLogos(LogosAPI*)` helper plus an `eventResponse(eventName, data)` signal for event forwarding. Public methods intended for RPC should be marked `Q_INVOKABLE` so Qt's meta-object system can invoke them remotely.

Each plugin ships a `metadata.json` referenced by `Q_PLUGIN_METADATA`. Required fields include `name`, `version`, `description`, `author`, `type` (`core`), `dependencies`, `capabilities`, and optional `include` entries for extra files (e.g., shared libs) that must be copied alongside the plugin. The core reads this metadata via `QPluginLoader` during discovery and uses it to populate the known plugins list and enforce dependency handling.

## 5. Core Library Usage

### 5.1 Basic Usage

**C API usage:**

```c
#include "logos_core.h"

int main(int argc, char *argv[]) {
    // Initialize
    logos_core_init(argc, argv);
    // Optional: switch to in-process Local mode for mobile/embedded builds
    // logos_core_set_mode(LOGOS_MODE_LOCAL);
    logos_core_set_plugins_dir("/path/to/plugins");
    
    // Start
    logos_core_start();
    
    // Load a module
    logos_core_load_plugin("chat");
    
    // Get loaded plugins
    char** loaded = logos_core_get_loaded_plugins();
    for (int i = 0; loaded[i] != NULL; i++) {
        printf("Loaded: %s\n", loaded[i]);
    }
    // Free the array
    free(loaded);
    
    // Run event loop
    int result = logos_core_exec();
    
    // Cleanup
    logos_core_cleanup();
    return result;
}
```

**C++ usage with SDK:**

```cpp
#include "logos_api.h"

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    
    // Optional: run everything in-process (Local mode) for mobile
    // logos_core_set_mode(LOGOS_MODE_LOCAL);
    
    // Set plugins directory
    logos_core_set_plugins_dir("/path/to/plugins");
    
    // Start core
    logos_core_start();
    
    // Create LogosAPI for module interaction
    LogosAPI logosAPI("app", nullptr);
    
    // Load and interact with modules
    logos_core_load_plugin("chat");
    LogosAPIClient* chatClient = logosAPI.getClient("chat");
    chatClient->invokeRemoteMethod("chat", "initialize");
    
    // Run event loop
    return app.exec();
}
```

## 6. Module Implementation

### 6.1 Overview

A complete module consists of four essential components:

1. **Interface Header** - Defines the module's public API contract
2. **Plugin Implementation** - Concrete implementation of the interface
3. **Metadata File** - Describes module properties and dependencies
4. **Build Configuration** - CMakeLists.txt for compilation

All modules must inherit from `PluginInterface` (found at `logos-liblogos/interface.h`) and implement the required lifecycle methods (`name()`, `version()`, `initLogos()`) and include the `eventResponse` signal for events. Methods exposed to other modules must be marked with `Q_INVOKABLE` to enable Qt's meta-object system to invoke them across process boundaries.

### 6.2 Required Files
    
#### Interface Header

The interface header defines your module's public API contract. It must inherit from `PluginInterface` and declare all methods that other modules can invoke remotely.

**Key Requirements:**
- Inherit from `PluginInterface` (provides `name()`, `version()`, `initLogos()`)
- Mark all public methods with `Q_INVOKABLE` for remote access
- Include `eventResponse` signal for event forwarding
- Use `Q_DECLARE_INTERFACE` macro for Qt's plugin system

```c++
// MyModuleInterface.h
#pragma once
#include <QtCore/QObject>
#include <QtCore/QJsonArray>
#include <QtCore/QStringList>
#include "interface.h"

class MyModuleInterface : public PluginInterface {
public:
    virtual ~MyModuleInterface() {}
    
    // Public API methods - must be Q_INVOKABLE for remote access
    Q_INVOKABLE virtual void doSomething(const QString &param) = 0;
    Q_INVOKABLE virtual QString processData(const QString &input) = 0;
    Q_INVOKABLE virtual QJsonArray getStatus() = 0;

    Q_INVOKABLE void initLogos(LogosAPI* logosAPIInstance);

signals:
    // Required for event forwarding between modules
    void eventResponse(const QString &eventName, const QVariantList &data);
};

// Register interface with Qt's meta-object system
#define MyModuleInterface_iid "org.logos.MyModuleInterface"
Q_DECLARE_INTERFACE(MyModuleInterface, MyModuleInterface_iid)
```

#### Plugin Implementation

The plugin class provides the concrete implementation of your interface. It must inherit from both `QObject` and your custom interface to enable Qt's plugin and remote object systems.

**Key Requirements:**
- Inherit from `QObject` and your interface
- Use `Q_PLUGIN_METADATA` with proper IID and metadata file
- Register interface with `Q_INTERFACES` macro
- Implement all pure virtual methods from the interface
- Store and use `LogosAPI*` for inter-module communication

```c++
// MyModulePlugin.h
#pragma once
#include <QtCore/QObject>
#include <QtCore/QJsonArray>
#include <QtCore/QTimer>
#include "MyModuleInterface.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_sdk.h"

class MyModulePlugin : public QObject, public MyModuleInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID MyModuleInterface_iid FILE "metadata.json")
    Q_INTERFACES(MyModuleInterface PluginInterface)

public:
    MyModulePlugin();
    ~MyModulePlugin();

    // PluginInterface implementation
    QString name() const override { return "my_module"; }
    QString version() const override { return "1.0.0"; }

    void MyModulePlugin::initLogos(LogosAPI* logosAPIInstance) {
        logosAPI = logosAPIInstance;
        logos = new LogosModules(logosAPI); // generated wrappers aggregator
        logos->core_manager.setEventSource(this); // enable trigger() helper
    }

    // Custom API implementation
    Q_INVOKABLE void doSomething(const QString &param) override {
        QString result = QString("Processed: %1").arg(param);
        QDateTime timestamp = QDateTime::currentDateTime();


        QVariantList eventData;
        eventData << result << timestamp.toString();
        logos->core_manager.trigger("dataProcessed", eventData);
    }

    Q_INVOKABLE QString processData(const QString &input) override {
        if (input.isEmpty()) {
            emit errorOccurred("Empty input provided", 1);
            return QString();
        }
        
        QString result = QString("Processed: %1").arg(input);
    
        // example: call another module using generated wrappers
        // pattern: logos.<module>.<method>(...)
        // e.g., logos->chat.sendMessage(channel, nick, result);

        // example subscribing to an event through the generated helper
        logos->chat.on("chatMessage", [this](const QVariantList& data) {
            qDebug() << data.value(0).toString();
        });

        return result;
    }
    
    Q_INVOKABLE QJsonArray getStatus() override {
        QJsonArray status;
        status.append(QJsonObject{
            {"active", true}, 
            {"connections", m_connections},
            {"subscribers", m_eventSubscribers.size()}
        });
        return status;
    }

```
    
#### Metadata File

Every module requires a `metadata.json` file that describes the module's properties, dependencies, and capabilities. This file is referenced by the `Q_PLUGIN_METADATA` macro and used by the core for module discovery and dependency resolution.

**Required Fields:**
- `name`: Unique module identifier (must match the value returned by `name()`)
- `version`: Semantic version string
- `description`: Human-readable description
- `author`: Module author or organization
- `type`: Module type (always "core" for this PoC)
- `category`: Module category for organization
- `main`: Main plugin class name
- `dependencies`: Array of required module names
- `capabilities`: Array of capabilities this module provides

**Example metadata.json:**
```json
{
  "name": "my_module",
  "version": "1.0.0",
  "description": "Example module demonstrating the plugin API",
  "author": "Logos Core Team",
  "type": "core",
  "category": "utility",
  "main": "my_module_plugin",
  "dependencies": ["core_manager"],
  "capabilities": ["data_processing", "event_handling"],
  "include": ["external_lib.so", "resources/"]
}
```

The core scans for metadata.json files during startup and validates dependencies before loading modules.

#### Build Configuration

Each module needs a `CMakeLists.txt` file to compile into a shared library. The build system must produce a `.so` (Linux), `.dylib` (macOS), or `.dll` (Windows) file that the core can dynamically load.

**Example CMakeLists.txt:**
```cmake
cmake_minimum_required(VERSION 3.16)
project(my_module)

# Find required Qt components
find_package(Qt6 REQUIRED COMPONENTS Core RemoteObjects)

# Enable Qt's automoc for meta-object code generation
set(CMAKE_AUTOMOC ON)

# Define the module as a shared library
add_library(my_module SHARED
    MyModulePlugin.cpp
    MyModulePlugin.h
    MyModuleInterface.h
)

# Optional: generate and consume typed wrappers (logos_sdk)
set(CPP_GENERATOR "${CMAKE_SOURCE_DIR}/../build/cpp-generator/bin/logos-cpp-generator")
set(REPO_ROOT "${CMAKE_SOURCE_DIR}/..")
set(PLUGINS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/modules")
set(METADATA_JSON "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json")

add_custom_target(run_cpp_generator_my_module
    COMMAND "${CPP_GENERATOR}" --metadata "${METADATA_JSON}" --module-dir "${PLUGINS_OUTPUT_DIR}"
    WORKING_DIRECTORY "${REPO_ROOT}"
    COMMENT "Running logos-cpp-generator for my_module"
    VERBATIM
)

# If your module calls other modules via generated wrappers, include the umbrella once
set(GENERATED_LOGOS_SDK_CPP ${CMAKE_CURRENT_SOURCE_DIR}/../../logos-cpp-sdk/cpp/generated/logos_sdk.cpp)
set_source_files_properties(${GENERATED_LOGOS_SDK_CPP} PROPERTIES GENERATED TRUE)
target_sources(my_module PRIVATE ${GENERATED_LOGOS_SDK_CPP})
target_include_directories(my_module PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../logos-cpp-sdk/cpp/generated)
add_dependencies(my_module run_cpp_generator_my_module)

# Link Qt libraries
target_link_libraries(my_module
    Qt6::Core
    Qt6::RemoteObjects
)

# Set target properties
set_target_properties(my_module PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    VERSION ${CMAKE_PROJECT_VERSION}
    SOVERSION 1
)

# Install the shared library to the modules directory
install(TARGETS my_module
    LIBRARY DESTINATION modules
    RUNTIME DESTINATION modules
)

# Install metadata file
install(FILES metadata.json
    DESTINATION modules
)
```

### 6.3 Plugin Development Gotchas and Best Practices

This section covers common issues and best practices when developing plugins, particularly around library dependencies and runtime loading.

#### 6.3.1 External Library Dependencies and Runtime Loading

**Problem**: When your plugin depends on external shared libraries (`.so`, `.dylib`, `.dll`), the dynamic loader must be able to find these libraries at runtime. Common issues include:

1. **Absolute paths in binaries**: Libraries compiled with absolute paths won't work when deployed to different systems
2. **Missing library search paths**: The plugin can't find its dependencies when loaded by the core
3. **Install name issues on macOS**: Libraries with incorrect install names cause loading failures

##### macOS Library Path Handling

On macOS, use `@rpath` for portable library references. Here's the pattern used in the wallet module:

```cmake
if(APPLE)
    # Set proper rpath for the plugin
    set_target_properties(my_module_plugin PROPERTIES
        INSTALL_RPATH "@loader_path"
        INSTALL_NAME_DIR "@rpath"
        BUILD_WITH_INSTALL_NAME_DIR TRUE)

    # If you have external libraries to copy and fix
    if(EXTERNAL_LIB_PATH)
        # Copy the library to the build directory
        add_custom_command(TARGET my_module_plugin PRE_LINK
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${EXTERNAL_LIB_PATH}
            ${CMAKE_BINARY_DIR}/modules/libexternal.dylib
            COMMENT "Copying external library to modules directory"
        )

        # Fix install names after build using a CMake script
        add_custom_command(TARGET my_module_plugin POST_BUILD
            COMMAND install_name_tool -id "@rpath/my_module_plugin.dylib" $<TARGET_FILE:my_module_plugin>
            COMMAND ${CMAKE_COMMAND} -DLIB_PATH=${CMAKE_BINARY_DIR}/modules/libexternal.dylib 
                    -DPLUGIN_PATH=${CMAKE_BINARY_DIR}/modules/my_module_plugin.dylib 
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/fix_install_names.cmake
            COMMENT "Updating library paths for macOS"
        )
    endif()
endif()
```

Create a `fix_install_names.cmake` script:
```cmake
# fix_install_names.cmake
if(EXISTS "${LIB_PATH}")
    message(STATUS "Fixing install name for external library: ${LIB_PATH}")
    execute_process(
        COMMAND install_name_tool -id "@rpath/libexternal.dylib" "${LIB_PATH}"
        RESULT_VARIABLE result
    )
    if(result)
        message(WARNING "Failed to update install name for external library")
    endif()
    
    # Update plugin to reference the library with @rpath
    execute_process(
        COMMAND install_name_tool -change "/old/absolute/path/libexternal.dylib" "@rpath/libexternal.dylib" "${PLUGIN_PATH}"
        RESULT_VARIABLE result
    )
    if(result)
        message(WARNING "Failed to update library reference in plugin")
    endif()
else()
    message(STATUS "External library not found, skipping install name update")
endif()
```

##### Linux Library Path Handling

On Linux, use `$ORIGIN` for relative library paths:

```cmake
else() # Linux
    # Set rpath to look in the same directory as the plugin
    set_target_properties(my_module_plugin PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        INSTALL_RPATH_USE_LINK_PATH FALSE)

    # Copy external library if it exists
    if(EXTERNAL_LIB_PATH)
        add_custom_command(TARGET my_module_plugin PRE_LINK
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${EXTERNAL_LIB_PATH}
            ${CMAKE_BINARY_DIR}/modules/libexternal.so
            COMMENT "Copying external library to modules directory"
        )
    endif()
endif()
```

#### 6.3.2 Testing Library Dependencies

Before deploying your plugin, verify that library dependencies are correctly resolved:

**macOS**:
```bash
# Check what libraries your plugin depends on
otool -L your_plugin.dylib

# Check the install name of a library
otool -D your_library.dylib

# Verify @rpath resolution
install_name_tool -id "@rpath/your_library.dylib" your_library.dylib
```

**Linux**:
```bash
# Check library dependencies
ldd your_plugin.so

# Check rpath settings
readelf -d your_plugin.so | grep RPATH
```

