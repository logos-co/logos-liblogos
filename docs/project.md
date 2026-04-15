# Project Description

## Project Structure

```
logos-liblogos/
├── CMakeLists.txt                       # Root CMake configuration
├── README.md                            # Project overview and build instructions
├── flake.nix                            # Nix flake configuration
├── flake.lock                           # Nix flake lock file
├── docs/
│   ├── index.md                         # Documentation index
│   ├── spec.md                          # High-level specification
│   └── project.md                       # This document
├── src/
│   ├── CMakeLists.txt                   # Source build configuration
│   ├── logos_core/                      # Core library implementation
│   │   ├── logos_core.h                 # C API header (public)
│   │   ├── logos_core.cpp               # C API implementation
│   │   ├── app_lifecycle.h/cpp          # Application lifecycle management
│   │   ├── plugin_manager.h/cpp         # Facade: orchestrates registry, launcher, resolver
│   │   ├── plugin_registry.h/cpp        # In-memory registry of discovered/loaded modules
│   │   ├── plugin_launcher.h/cpp        # Spawns and manages logos_host subprocesses
│   │   ├── dependency_resolver.h/cpp    # Topological sort with circular dependency detection
│   │   ├── process_manager.h/cpp        # Boost.Process-based subprocess management
│   │   └── qt/                          # Qt-specific implementations
│   │       └── qt_app_context.h/cpp     # QCoreApplication management
│   └── logos_host/                      # Module subprocess host
│       ├── logos_host.cpp               # Host entry point
│       ├── command_line_parser.h/cpp    # CLI argument parsing (--name, --path)
│       ├── plugin_initializer.h/cpp     # Plugin loading and token setup
│       └── qt/                          # Qt-specific host implementations
│           ├── qt_app.h/cpp             # Qt application setup for host
│           └── qt_token_receiver.h/cpp  # Auth token reception via local socket
├── tests/                               # Google Test suite
│   ├── CMakeLists.txt                   # Test build configuration
│   ├── test_app_lifecycle.cpp           # AppLifecycle tests
│   ├── test_plugin_manager.cpp          # PluginManager + PluginRegistry tests
│   ├── test_process_manager.cpp         # ProcessManager lifecycle and subprocess tests
│   ├── test_process_stats.cpp           # ProcessStats tests (external process-stats lib)
│   └── test_token_exchange.cpp          # Token exchange via Unix domain socket tests
├── nix/                                 # Nix build modules
│   ├── default.nix                      # Common configuration (deps, flags, metadata)
│   ├── build.nix                        # Shared build derivation
│   ├── bin.nix                          # Binary extraction (logos_host + runtime libs)
│   ├── lib.nix                          # Library extraction (liblogos_core)
│   ├── include.nix                      # Header installation
│   ├── modules.nix                      # Bundled built-in modules
│   └── tests.nix                        # Test suite build
└── .github/
    └── workflows/
        └── ci.yml                       # GitHub Actions CI workflow
```

## Stack, Frameworks & Dependencies

| Component | Purpose |
|-----------|---------|
| **C++17** | Implementation language |
| **CMake 3.14+** | Build system |
| **Qt 6** (Core, RemoteObjects) | Event loop, plugin system, IPC, meta-object system |
| **Boost** (Process, Asio) | Subprocess management and async I/O |
| **nlohmann_json** | JSON parsing/serialization (replaces Qt JSON internally) |
| **CLI11** | Command-line argument parsing (logos_host) |
| **zstd** | Compression (build dependency) |
| **Google Test** | Unit testing framework |
| **Nix** | Package management and reproducible builds |

### External Logos Dependencies

| Dependency | Purpose |
|------------|---------|
| **[logos-cpp-sdk](https://github.com/logos-co/logos-cpp-sdk)** | C++ client library (LogosAPI, LogosAPIClient, TokenManager, PluginInterface) |
| **[logos-module](https://github.com/logos-co/logos-module)** | Module library (metadata extraction, plugin loading utilities) |
| **[logos-capability-module](https://github.com/logos-co/logos-capability-module)** | Built-in capability authorization module |
| **[process-stats](https://github.com/logos-co/process-stats)** | CPU and memory monitoring for module processes |
| **[logos-package-manager](https://github.com/logos-co/logos-package-manager-module)** | Package management library for installed module discovery |
| **[logos-nix](https://github.com/logos-co/logos-nix)** | Common Nix tooling and nixpkgs pin |

## Core Modules

### AppLifecycle

**Files:** `src/logos_core/app_lifecycle.h`, `src/logos_core/app_lifecycle.cpp`

**Purpose:** Application lifecycle management. Delegates Qt-specific work to `QtAppContext`.

**API (namespace `AppLifecycle`):**

| Method | Description |
|--------|-------------|
| `init(argc, argv)` | Initialize global state, create QCoreApplication if needed |
| `start()` | Discover plugins, initialize capability module |
| `exec() → int` | Run the Qt event loop |
| `cleanup()` | Terminate all module processes, clean up state |
| `processEvents()` | Process Qt events without blocking |

### PluginManager

**Files:** `src/logos_core/plugin_manager.h`, `src/logos_core/plugin_manager.cpp`

**Purpose:** Thin facade that orchestrates `PluginRegistry`, `PluginLauncher`, and `DependencyResolver`. Provides the C++-level API for module lifecycle management. Each module runs in a separate `logos_host` process for isolation.

**Thread safety:** `loadPlugin`, `loadPluginWithDependencies`, and `unloadPlugin` are serialised by a static `loadMutex()` (one load/unload at a time). `discoverInstalledModules` delegates to `PluginRegistry` which has its own reader-writer lock.

**API (namespace `PluginManager`):**

| Method | Description |
|--------|-------------|
| `registry() → PluginRegistry&` | Access the shared plugin registry |
| `setPluginsDir(path)` | Set the primary plugin directory (clears existing) |
| `addPluginsDir(path)` | Add an additional plugin directory |
| `setPersistenceBasePath(path)` | Set base directory for module instance persistence |
| `discoverInstalledModules()` | Scan all plugin directories and register discovered modules |
| `processPlugin(path) → QString` | Extract metadata from a module file, register as known |
| `processPluginCStr(path) → char*` | C-string variant of processPlugin |
| `loadPlugin(name) → bool` | Load a module (spawns `logos_host` process, sends auth token) |
| `loadPluginWithDependencies(name) → bool` | Resolve dependency tree, load in topological order |
| `initializeCapabilityModule() → bool` | Load the built-in capability module if available |
| `unloadPlugin(name) → bool` | Terminate module process and update registry |
| `terminateAll()` | Terminate all running module processes |
| `resolveDependencies(modules) → QStringList` | Topological sort with circular dependency detection |
| `getLoadedPluginsCStr() → char**` | Return loaded module names as null-terminated C string array |
| `getKnownPluginsCStr() → char**` | Return known module names as null-terminated C string array |
| `isPluginLoaded(name) → bool` | Check if a module is currently loaded |
| `getPluginProcessIds() → QHash` | Return module name → process ID mappings |

### PluginRegistry

**Files:** `src/logos_core/plugin_registry.h`, `src/logos_core/plugin_registry.cpp`

**Purpose:** In-memory registry of discovered and loaded modules. Stores plugin paths, dependencies, and load state. All public methods are thread-safe: mutating methods acquire a `std::unique_lock` on an internal `std::shared_mutex`; read-only methods acquire a `std::shared_lock`, allowing concurrent reads.

**Data:**
- `PluginInfo` struct — holds `path`, `dependencies` (QStringList), `loaded` flag
- `QHash<QString, PluginInfo> m_plugins` — plugin database keyed by name
- `QStringList m_pluginsDirs` — configured plugin directories
- `std::shared_mutex m_mutex` — reader-writer lock protecting all fields

**API (class `PluginRegistry`):**

| Method | Description |
|--------|-------------|
| `setPluginsDir(dir)` | Clear and set single plugin directory |
| `addPluginsDir(dir)` | Add to plugin directory list |
| `pluginsDirs() → QStringList` | Return configured directories |
| `discoverInstalledModules()` | Scan directories, parse manifest.json files |
| `processPlugin(path) → QString` | Extract metadata from module file, register as known |
| `registerPlugin(name, path, deps)` | Manually register a plugin |
| `registerDependencies(name, deps)` | Set dependencies for a known plugin |
| `isKnown(name) → bool` | Plugin exists in registry |
| `pluginPath(name) → QString` | Get file path for a known plugin |
| `pluginDependencies(name) → QStringList` | Get dependency list for a plugin |
| `knownPluginNames() → QStringList` | All discovered module names |
| `isLoaded(name) → bool` | Plugin is currently running |
| `markLoaded(name)` / `markUnloaded(name)` | Update load state |
| `loadedPluginNames() → QStringList` | Currently running module names |
| `clearLoaded()` | Clear all loaded state |
| `clear()` | Reset entire registry |

### PluginLauncher

**Files:** `src/logos_core/plugin_launcher.h`, `src/logos_core/plugin_launcher.cpp`

**Purpose:** Spawn and manage module subprocesses. Delegates to the process manager (Boost.Process v2 / Boost.Asio) for subprocess operations.

**API (namespace `PluginLauncher`):**

| Method | Description |
|--------|-------------|
| `launch(name, path, dirs, instancePersistencePath, onTerminated) → bool` | Spawn `logos_host` process for a module |
| `sendToken(name, token) → bool` | Send auth token to module process via stdin |
| `terminate(name)` | Kill a specific module process |
| `terminateAll()` | Kill all module processes |
| `hasProcess(name) → bool` | Check if a process exists for this module |
| `getAllProcessIds() → QHash` | Map module names to process IDs |

### DependencyResolver

**Files:** `src/logos_core/dependency_resolver.h`, `src/logos_core/dependency_resolver.cpp`

**Purpose:** Compute topological sort of module dependencies using Kahn's algorithm. Detects circular dependencies and missing modules.

**API (namespace `DependencyResolver`):**

| Method | Description |
|--------|-------------|
| `resolve(requested, isKnown, getDependencies) → QStringList` | Returns modules in load order (dependencies first) |

Takes callback functions (`IsKnownFn`, `GetDependenciesFn`) so it has no coupling to the registry implementation.

### Qt Integration Layer

**Files:** `src/logos_core/qt/qt_app_context.h/cpp`

**Purpose:** Isolates Qt-specific application lifecycle code behind internal interfaces.

- **QtAppContext** — Creates/manages `QCoreApplication`, runs event loop, processes events

### Process Manager

**Files:** `src/logos_core/process_manager.h`, `src/logos_core/process_manager.cpp`

**Purpose:** Manages module subprocesses using Boost.Process v2 and Boost.Asio. Replaces the former Qt-based `QProcess` implementation.

- Uses `boost::process::v2::process` for subprocess spawning and `boost::asio::io_context` for async I/O
- Background `io_context` thread with work guard for non-blocking async read and wait callbacks
- Async read loop for stdout/stderr with line buffering
- Synchronous kill with graceful SIGTERM → SIGKILL escalation (5s timeout)
- Unix domain socket for token delivery (matches previous `QLocalSocket` behavior)
- A `std::mutex` (`s_processesMutex`) protects the `s_processes` map against concurrent access
- Shared pointer-based lifetime management for safe async callback handling

**API (namespace `QtProcessManager`):**

| Method | Description |
|--------|-------------|
| `startProcess(name, executable, arguments, callbacks) → bool` | Launch a subprocess with async output monitoring |
| `sendToken(name, token) → bool` | Send auth token via Unix domain socket |
| `terminateProcess(name)` | Gracefully terminate a specific process |
| `terminateAll()` | Terminate all managed processes |
| `hasProcess(name) → bool` | Check if a process entry exists |
| `getProcessId(name) → int64_t` | Get PID for a named process |
| `getAllProcessIds() → unordered_map` | Map all process names to PIDs |
| `registerProcess(name)` | Register a placeholder process entry |
| `clearAll()` | Clear all process entries |

### ProcessStats (external dependency)

**Source:** [process-stats](https://github.com/logos-co/process-stats) library (linked as a static dependency)

**Purpose:** CPU and memory monitoring for loaded module processes.

**API (namespace `ProcessStats`):**

| Method | Description |
|--------|-------------|
| `getProcessStats(pid) → ProcessStatsData` | Return CPU %, CPU time, and memory usage for a process |
| `getModuleStats(processIds) → char*` | Return JSON array of stats for all provided module processes |

### LogosHost

**Files:** `src/logos_host/logos_host.cpp`, `src/logos_host/command_line_parser.h/cpp`, `src/logos_host/plugin_initializer.h/cpp`, `src/logos_host/qt/qt_app.h/cpp`, `src/logos_host/qt/qt_token_receiver.h/cpp`

**Purpose:** Lightweight subprocess that loads a single module. Parses `--name` and `--path` arguments, loads the plugin, authenticates via token from the core, registers the module with the remote object registry, and runs the Qt event loop.

## C API

The public C API (`logos_core.h`) is the only exported interface. All functions use `LOGOS_CORE_EXPORT` for shared library visibility.

**Lifecycle:**

| Function | Description |
|----------|-------------|
| `logos_core_init(argc, argv)` | Initialize the library |
| `logos_core_start()` | Discover modules and initialize capability module |
| `logos_core_exec() → int` | Run the Qt event loop |
| `logos_core_cleanup()` | Terminate all modules and clean up |
| `logos_core_process_events()` | Process Qt events without blocking |

**Plugin Management:**

| Function | Description |
|----------|-------------|
| `logos_core_set_plugins_dir(dir)` | Set primary plugin directory |
| `logos_core_add_plugins_dir(dir)` | Add additional plugin directory |
| `logos_core_set_persistence_base_path(path)` | Set base directory for module instance persistence |
| `logos_core_load_plugin(name) → int` | Load a module (1 = success, 0 = failure) |
| `logos_core_load_plugin_with_dependencies(name) → int` | Load module and dependencies in order |
| `logos_core_unload_plugin(name) → int` | Unload a module |
| `logos_core_process_plugin(path) → char*` | Process plugin file, return name (caller frees) |
| `logos_core_refresh_plugins()` | Re-scan plugin directories |

**Queries:**

| Function | Description |
|----------|-------------|
| `logos_core_get_loaded_plugins() → char**` | Null-terminated array of loaded names (caller frees) |
| `logos_core_get_known_plugins() → char**` | Null-terminated array of known names (caller frees) |
| `logos_core_get_module_stats() → char*` | JSON array of CPU/memory stats (caller frees) |
| `logos_core_get_token(key) → char*` | Get auth token by key (caller frees) |

### Thread Safety

| Category | Guarantee |
|----------|-----------|
| `logos_core_load_plugin`, `logos_core_load_plugin_with_dependencies`, `logos_core_unload_plugin` | Serialised by a single internal mutex — safe to call concurrently from multiple threads |
| `logos_core_get_known_plugins`, `logos_core_get_loaded_plugins` | Protected by a shared reader-writer lock — safe to call concurrently with each other and with the mutating functions above |
| `logos_core_refresh_plugins` | Protected by `PluginRegistry`'s reader-writer lock (write side) — safe for concurrent registry access but not serialised against load/unload |
| `logos_core_init`, `logos_core_start`, `logos_core_cleanup` | Not thread-safe — must be called from a single thread during startup/shutdown |

## Build Artifacts

| Artifact | Description |
|----------|-------------|
| `liblogos_core.{so,dylib,dll}` | Core shared library (C API) |
| `logos_host` | Module subprocess host binary |
| `logos_core_tests` | Google Test suite |

## Operational

### Nix (Recommended)

Nix provides reproducible builds with all dependencies managed automatically.

**Build everything (binaries + libraries + headers):**
```bash
nix build
```

The result includes:
- `result/bin/logos_host` — Module host binary
- `result/lib/liblogos_core.{so,dylib}` — Core library
- `result/include/` — Headers (logos_core.h, interface.h)

**Build individual components:**
```bash
nix build '.#logos-liblogos-bin'       # Binaries only
nix build '.#logos-liblogos-lib'       # Libraries only
nix build '.#logos-liblogos-include'   # Headers only
nix build '.#logos-liblogos-tests'     # Test suite
nix build '.#logos-liblogos-modules'   # Built-in modules
nix build '.#portable'                 # Portable variant (LOGOS_PORTABLE_BUILD=ON)
```

**Run tests:**
```bash
nix build '.#logos-liblogos-tests'
./result/bin/logos_core_tests

# Run specific tests
./result/bin/logos_core_tests --gtest_filter=AppLifecycleTest.*

# List all tests
./result/bin/logos_core_tests --gtest_list_tests
```

**Development shell:**
```bash
nix develop
```

**Override local dependencies:**
```bash
nix build --override-input logos-cpp-sdk path:../logos-cpp-sdk
```

### CMake

**Prerequisites:**
- CMake 3.14+
- C++17 compatible compiler
- Qt 6 with Core and RemoteObjects modules
- Boost (with Process component)
- nlohmann_json
- CLI11
- Google Test (fetched via FetchContent if not system-installed)

**Build:**
```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

The `logos_host` binary will be in `build/bin/` and `liblogos_core` in `build/lib/`.

**Build with tests:**
```bash
cmake -DLGX_BUILD_TESTS=ON ..
make -j$(nproc)
ctest --output-on-failure
```

**Note:** CMake expects the logos-cpp-sdk and logos-module libraries to be available. It searches for a pre-built SDK first (lib/include), then falls back to source (cpp/CMakeLists.txt).

### Dev vs Portable Builds

Controlled by the `LOGOS_PORTABLE_BUILD` CMake flag:

- **Dev** (default): Looks for LGX variants with `-dev` suffix (e.g., `linux-amd64-dev`)
- **Portable** (`-DLOGOS_PORTABLE_BUILD=ON`): Looks for variants without suffix (e.g., `linux-amd64`)

Build portable with Nix: `nix build '.#portable'`

## Consumers

`logos-liblogos` is a library consumed by two frontends:

- **[logos-basecamp](https://github.com/logos-co/logos-basecamp)** — Desktop GUI application shell
- **[logos-logoscore-cli](https://github.com/logos-co/logos-logoscore-cli)** — Headless CLI runtime (`logoscore`)

## Examples

### Basic C API Usage

```c
#include "logos_core.h"

int main(int argc, char *argv[]) {
    logos_core_init(argc, argv);
    logos_core_set_plugins_dir("/path/to/plugins");

    logos_core_start();
    logos_core_load_plugin("chat");

    char** loaded = logos_core_get_loaded_plugins();
    for (int i = 0; loaded[i] != NULL; i++) {
        printf("Loaded: %s\n", loaded[i]);
    }
    free(loaded);

    int result = logos_core_exec();
    logos_core_cleanup();
    return result;
}
```

### Loading with Dependencies

```c
// Resolves the dependency tree and loads in correct order
logos_core_load_plugin_with_dependencies("my_module");
```

## Continuous Integration

GitHub Actions workflow (`.github/workflows/ci.yml`) runs on every push/PR to `master`:

1. Checkout code
2. Install Nix with flakes enabled
3. Use cachix cache (`logos-co`)
4. Build: `nix build .#logos-liblogos-tests`
5. Run: `./result/bin/logos_core_tests`

## Supported Platforms

- macOS (aarch64-darwin, x86_64-darwin)
- Linux (aarch64-linux, x86_64-linux)
