# Logos Core Library

## Overall Description

Logos Core is a modular runtime platform for hosting and orchestrating independently developed modules (plugins). It provides a C-API shared library (`liblogos_core`) and a pluggable runtime system that together enable a plug-in-based architecture for decentralised applications. The default runtime (`SubprocessManager`) spawns a `logos_host_qt` process per module, but the architecture allows registering alternative runtimes for different module formats.

The platform is designed to:
- Load, start, stop, and introspect modules at runtime
- Isolate each module in its own process for robustness and security
- Provide transparent inter-module RPC via a remote object registry
- Support token-based authentication for secure module-to-module communication
- Expose a C API so that host applications in any language can drive the runtime

## Definitions & Acronyms

| Term | Definition |
|------|------------|
| **Module** | An independently developed plugin that implements `PluginInterface` and is dynamically loaded by the core |
| **Core Library** | `liblogos_core` — the shared library that provides the C API for module management |
| **Module Host** | `logos_host_qt` — a lightweight executable that loads a single Qt module in its own process (a `logos_host` symlink exists for backward compatibility) |
| **Module Runtime** | An abstract interface (`ModuleRuntime`) that encapsulates a strategy for loading and managing modules (e.g. subprocess, in-process, WASM) |
| **Runtime Registry** | The central registry (`RuntimeRegistry`) that selects the appropriate `ModuleRuntime` for a given module based on its format descriptor |
| **Core Manager** | A built-in module that exposes core functionality via RPC, allowing modules to manage the core without linking against the C API |
| **Capability Module** | A built-in module that handles authorization tokens for inter-module communication |
| **RPC** | Remote Procedure Call — the mechanism by which modules invoke methods on each other |
| **IPC** | Inter-Process Communication — the underlying transport |
| **Token** | A UUID-based authentication credential issued by the core or capability module for securing RPC calls |
| **SDK** | The [logos-cpp-sdk](https://github.com/logos-co/logos-cpp-sdk) — client library that abstracts connection management, token handling, and asynchronous invocation |

## Domain Model

### System Architecture

At a high level, the Logos Core consists of:

**Core Library** — The C/C++ shared library (`liblogos_core`) that provides the API functions for lifecycle management, module loading/unloading, and introspection.

**Runtime Registry** — A central registry of `ModuleRuntime` implementations. When a module is loaded, the registry selects the first runtime whose `canHandle()` returns true for the module's descriptor. This decouples the core from any specific loading mechanism.

**Module Runtimes** — Pluggable implementations of the `ModuleRuntime` interface. The default `SubprocessManager` spawns `logos_host_qt` processes, but alternative runtimes (in-process, WASM, etc.) can be registered.

**Core Manager** — A built-in module that runs in the core process and exposes core functionality as RPC methods, allowing remote modules to manage the core without linking against the C API directly.

**Module Host** — A lightweight executable (`logos_host_qt`) that loads a single Qt module in its own process. It communicates with the core over a local socket to receive an authentication token and registers the module's object with the remote registry.

**Capability Module** — A built-in module that handles authorization for inter-module communication by issuing tokens and notifying both communicating parties.

**Remote Object Registry** — A registry that maintains a mapping of module names to remote object replicas and forwards method calls/events.

### Process Architecture

Each module runs in its own process for isolation:

```
┌──────────────────────────────────────────────────────┐
│  Host Application                                    │
│  ┌────────────────────────────────────────────────┐  │
│  │  liblogos_core                                 │  │
│  │  ├─ RuntimeRegistry                            │  │
│  │  │   └─ SubprocessManager (default runtime)     │  │
│  │  ├─ Core Manager (built-in module)             │  │
│  │  ├─ Capability Module (built-in module)        │  │
│  │  └─ Remote Object Registry                     │  │
│  └────────────────────────────────────────────────┘  │
│           │ IPC (local socket)                        │
│     ┌─────┼─────────┐                                │
│     ▼     ▼         ▼                                 │
│  ┌─────┐ ┌─────┐ ┌─────┐                             │
│  │host │ │host │ │host │  (logos_host_qt processes)   │
│  │mod A│ │mod B│ │mod C│                               │
│  └─────┘ └─────┘ └─────┘                             │
└──────────────────────────────────────────────────────┘
```

- The core uses a `RuntimeRegistry` to select the appropriate `ModuleRuntime` for each module
- The default `SubprocessManager` runtime spawns a `logos_host_qt` process per module
- Modules with `format == "qt-plugin"` or no explicit format are handled by `SubprocessManager`
- Communication happens via the Logos API (currently uses Qt Remote Objects over local sockets)
- Faulty or untrusted modules cannot crash the core or other modules
- Modules can be written in different languages as long as they implement the RPC protocol
- Alternative runtimes can be registered for different module formats (e.g. in-process, WASM)

### Token-Based Authentication

Since the remote object registry has no built-in security mechanisms, all RPC calls require an authentication token. This is transparent to module developers when using the SDK:

1. **Core → Module**: When a module is loaded, the core generates a UUID token and sends it to the module process via local socket. The module uses this token to authenticate calls from the core.
2. **Module → Module**: When modules need to communicate, they request authorization from the Capability Module, which issues a token and notifies both parties. The modules then use this token for subsequent requests.
3. **Token Storage**: Each module stores tokens in a thread-safe `TokenManager` (part of the SDK). `ModuleProxy` validates tokens before dispatching method calls.

### Module Metadata

Every module ships a `metadata.json` referenced by Qt's `Q_PLUGIN_METADATA` macro. Required fields:

| Field | Purpose |
|-------|---------|
| `name` | Unique module identifier (must match `name()` return value) |
| `version` | Semantic version string |
| `description` | Human-readable description |
| `author` | Module author or organization |
| `type` | Module type (e.g., `"core"`) |
| `category` | Module category for organization |
| `main` | Main plugin class name |
| `dependencies` | Array of required module names |
| `capabilities` | Array of capabilities this module provides |
| `include` | Optional array of extra files (shared libs, resources) to bundle |

## Features & Requirements

### Module Lifecycle

#### Discovery

1. Core scans configured plugin directories for `.so`, `.dylib`, or `.dll` files
2. For each file, metadata is extracted via `QPluginLoader`
3. Modules are added to the "known" list without being loaded
4. Multiple plugin directories can be configured

#### Loading

1. Core locates the plugin file for the requested module name
2. Core resolves dependencies and loads them first (topological sort with circular dependency detection)
3. If a persistence base path is configured, core resolves an instance ID and persistence directory for the module (reusing an existing instance or creating a new one)
4. Core builds a `ModuleDescriptor` (name, path, format, plugin dirs, persistence path)
5. Core asks the `RuntimeRegistry` to `select()` a runtime for the descriptor (the default `SubprocessManager` handles `"qt-plugin"` format and modules with no explicit format)
6. The selected runtime's `load()` spawns the appropriate process (e.g. `logos_host_qt` for the Qt subprocess runtime)
7. Core generates a UUID authentication token
8. Core sends the token to the module via the runtime's `sendToken()`
9. Host process loads the plugin and calls `initLogos(LogosAPI*)`
10. The `LogosAPI` instance exposes `modulePath`, `instanceId`, and `instancePersistencePath` as properties
11. Host process registers the plugin with the remote object registry
12. Core waits for registration and records the module as loaded (along with the runtime and handle)

#### Unloading

1. The module's host process is terminated
2. The module is removed from the loaded modules list
3. Associated tokens and state are cleaned up

#### Cascade Unloading

`logos_core_unload_plugin_with_dependents()` unloads the named module together with every currently loaded module that transitively depends on it. Teardown order is leaves-first (dependents before dependencies) so no process is left briefly pointing at a terminated parent. The call is serialised with ordinary load/unload operations under a single lock span — a late-arriving load cannot interleave between tearing down the dependents and the target.

### Dependency Resolution

- Dependencies are declared in each module's `metadata.json`
- `logos_core_load_plugin_with_dependencies()` performs topological sort
- Circular dependencies are detected and reported as errors
- Missing dependencies produce warnings
- Dependencies are loaded in correct order before the requesting module
- The core maintains an in-process dependency graph with both forward and reverse edges. The reverse edges are re-derived from the forward edges at the tail of every discovery or metadata-processing pass, so cascade unload and dependent queries answer from memory without re-reading manifests from disk.

### Process Monitoring

- CPU percentage, CPU time, and memory usage tracked per module process
- Statistics returned as JSON via `logos_core_get_module_stats()`
- Core Manager process is excluded from stats
- Not available on iOS

### Thread Safety

The C API is designed to be safe for use from multi-threaded host applications:

- **Load/unload operations** (`load_plugin`, `load_plugin_with_dependencies`, `unload_plugin`, `unload_plugin_with_dependents`) are serialised — only one runs at a time, so rapid concurrent load/unload cycles on the same or different modules do not produce data races. The cascade variant holds the lock for its full leaves-first teardown.
- **Read-only queries** (`get_known_plugins`, `get_loaded_plugins`) use a shared reader-writer lock and may execute concurrently with each other and with load/unload operations.
- **Plugin discovery** (`refresh_plugins`) is protected by the registry's own write lock.
- **Lifecycle functions** (`init`, `start`, `cleanup`) are not thread-safe and must be called from a single thread.

### Dev vs Portable Builds

The platform supports two build variants:

- **Dev build** (default): Plugin loading looks for LGX variants with `-dev` suffix (e.g., `linux-amd64-dev`). Used in Nix/development environments.
- **Portable build**: Looks for portable variants without suffix (e.g., `linux-amd64`). Used in self-contained distributed applications.

## API Description

### Core Lifecycle

| Function | Purpose |
|----------|---------|
| `logos_core_init(argc, argv)` | Initialize global state, optionally set plugin directory. Creates a QCoreApplication if one does not exist. |
| `logos_core_set_plugins_dir(path)` | Set the plugin directory. Must be called before starting. |
| `logos_core_add_plugins_dir(path)` | Add an additional plugin directory to scan. |
| `logos_core_start()` | Scan plugin directories, process metadata, create Core Manager, load built-in modules, start remote object registry. |
| `logos_core_exec()` | Run the Qt event loop. Returns when the application exits. |
| `logos_core_cleanup()` | Unload all modules, stop processes, clean up global state. |
| `logos_core_process_events()` | Process Qt events without blocking, for integration with external event loops. |

### Plugin Management

| Function | Purpose |
|----------|---------|
| `logos_core_get_loaded_plugins() → char**` | Return null-terminated array of loaded module names. Caller must free. |
| `logos_core_get_known_plugins() → char**` | Return null-terminated array of all discovered modules. Caller must free. |
| `logos_core_load_plugin(name) → int` | Load a module by name. Returns 1 on success, 0 on failure. |
| `logos_core_load_plugin_with_dependencies(name) → int` | Load a module and all its dependencies in correct order. Returns 1 if all succeed. |
| `logos_core_unload_plugin(name) → int` | Terminate the module's process and remove it. Returns 1 on success. |
| `logos_core_unload_plugin_with_dependents(name) → int` | Cascade unload: terminate the module together with every currently loaded transitive dependent, leaves-first. Returns 1 only if every step succeeded. |
| `logos_core_get_module_dependencies(name, recursive) → char**` | Return null-terminated array of modules that `name` depends on (forward edges). With `recursive=true`, walks the forward dependency graph transitively via BFS. Unknown names yield an empty array. Caller must free. |
| `logos_core_get_module_dependents(name, recursive) → char**` | Return null-terminated array of modules that depend on `name` (reverse edges). With `recursive=true`, walks the reverse dependency graph transitively via BFS. Unknown names yield an empty array. Caller must free. |
| `logos_core_process_plugin(path) → char*` | Read a module file's metadata and register it as known without loading. Returns the module name or NULL. Caller must free. |

### Token and Monitoring

| Function | Purpose |
|----------|---------|
| `logos_core_get_token(key) → char*` | Return the auth token for a key. Caller must free. NULL if not found. |
| `logos_core_get_module_stats() → char*` | Return JSON array of CPU/memory stats per loaded module. Caller must free. Not available on iOS. |

### Core Manager Module (RPC Surface)

The Core Manager is a built-in module exposing core functionality to remote modules:

| Method | Purpose |
|--------|---------|
| `setPluginsDirectory(directory)` | Set the module search directory. |
| `start()` | Start the core's registry and load built-in modules. |
| `cleanup()` | Unload all modules and shut down. |
| `getLoadedPlugins() → std::vector<std::string>` | Return names of loaded modules. |
| `getKnownPlugins() → QJsonArray` | Return all known modules with `loaded` flag. |
| `loadPlugin(name) → bool` | Load a plugin by name. |
| `unloadPlugin(name) → bool` | Unload a plugin by name. |
| `processPlugin(filePath) → std::string` | Read a plugin file's metadata and register it. |
| `getPluginMethods(name) → QJsonArray` | Introspect a module's methods via Qt meta-object system. |

## Module Implementation

A complete module must implement the Logos Module interface.

### Inter-Module Communication

Modules communicate using the SDK's generated C++ wrappers:

```
logos-><module_name>.<method>(args...)       // call a remote method
logos-><module_name>.on("event", callback)   // subscribe to events
logos-><module_name>.trigger("event", data)  // emit an event
```

The SDK abstracts away registry lookup, token management, and async invocation.

## Supported Platforms

- macOS (aarch64-darwin, x86_64-darwin)
- Linux (aarch64-linux, x86_64-linux)

## Future Work

- **Signature support** — Signing and verifying module packages
- **Additional runtimes** — Register alternative `ModuleRuntime` implementations (e.g. in-process, WASM) alongside the default `SubprocessManager`
- **Cross-language modules** — Modules in languages other than C++
- **Move away from Qt** — Logos API will move away from Qt. Process management has been migrated from Qt (`QProcess`) to Boost.Process v2, and Qt container/utility types (`QString`, `QStringList`, `QHash`, `QDir`, `QFile`, `QUuid`) have been replaced with standard C++ and Boost equivalents (`std::string`, `std::vector`, `std::unordered_map`, `std::filesystem`, `boost::uuids`). The runtime abstraction (`ModuleRuntime` / `RuntimeRegistry`) decouples the core from any specific loading strategy. Remaining Qt dependencies (event loop, plugin loading, remote objects) are isolated in `SubprocessManager` and the host binary.
