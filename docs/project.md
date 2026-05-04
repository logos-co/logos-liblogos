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
│   │   ├── module_manager.h/cpp         # Facade: orchestrates registry, runtime registry, resolver
│   │   ├── module_registry.h/cpp        # In-memory registry of discovered/loaded modules
│   │   ├── dependency_resolver.h/cpp    # Topological sort with circular dependency detection
│   │   ├── module_runtime.h             # Abstract ModuleRuntime interface (Qt-free)
│   │   └── runtime_registry.h/cpp       # Registry of ModuleRuntime implementations
│   └── runtimes/                        # Runtime implementations
│       └── runtime_qt/                  # Qt subprocess runtime + host binary
│           ├── subprocess_manager.h/cpp     # ModuleRuntime impl + Boost.Process subprocess management
│           ├── logos_host.cpp               # Host entry point (logos_host_qt binary)
│           ├── command_line_parser.h/cpp    # CLI argument parsing (--name, --path)
│           ├── module_initializer.h/cpp     # Module loading and token setup
│           └── qt/                          # Qt-specific host implementations
│               ├── qt_app.h/cpp             # Qt application setup for host
│               └── qt_token_receiver.h/cpp  # Auth token reception via local socket
├── tests/                               # Google Test suite
│   ├── CMakeLists.txt                   # Test build configuration
│   ├── test_app_lifecycle.cpp           # C API lifecycle tests (init, exec, cleanup, processEvents)
│   ├── test_module_manager.cpp          # ModuleManager + ModuleRegistry tests
│   ├── test_subprocess_manager.cpp      # SubprocessManager lifecycle and subprocess tests
│   ├── test_runtime_registry.cpp        # RuntimeRegistry selection and fan-out tests
│   ├── test_module_runtime_abstraction.cpp  # End-to-end runtime abstraction tests (FakeRuntime)
│   ├── test_dependency_resolver.cpp     # DependencyResolver tests
│   ├── test_process_stats.cpp           # ProcessStats tests (external process-stats lib)
│   ├── test_token_exchange.cpp          # Token exchange via Unix domain socket tests
│   └── qt_test_adapter.h               # Qt test utilities/adapter header
├── nix/                                 # Nix build modules
│   ├── default.nix                      # Common configuration (deps, flags, metadata)
│   ├── build.nix                        # Shared build derivation
│   ├── bin.nix                          # Binary extraction (logos_host_qt + runtime libs)
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
| **Qt 6** (Core, RemoteObjects) | Event loop, module system, IPC, meta-object system |
| **Boost** (Process, Asio, Uuid) | Subprocess management, async I/O, and UUID generation |
| **nlohmann_json** | JSON parsing/serialization (replaces Qt JSON internally) |
| **OpenSSL** | Required transitively by the SDK's plain-C++ TCP+TLS transport |
| **CLI11** | Command-line argument parsing (logos_host) |
| **zstd** | Compression (build dependency) |
| **spdlog** | Structured logging |
| **Google Test** | Unit testing framework |
| **Nix** | Package management and reproducible builds |

### External Logos Dependencies

| Dependency | Purpose |
|------------|---------|
| **[logos-cpp-sdk](https://github.com/logos-co/logos-cpp-sdk)** | C++ client library (LogosAPI, LogosAPIClient, TokenManager, PluginInterface) |
| **[logos-module](https://github.com/logos-co/logos-module)** | Module library (metadata extraction, module loading utilities) |
| **[logos-capability-module](https://github.com/logos-co/logos-capability-module)** | Built-in capability authorization module |
| **[process-stats](https://github.com/logos-co/process-stats)** | CPU and memory monitoring for module processes |
| **[logos-package-manager](https://github.com/logos-co/logos-package-manager-module)** | Package management library for installed module discovery |
| **[logos-nix](https://github.com/logos-co/logos-nix)** | Common Nix tooling and nixpkgs pin |

## Core Modules

### ModuleManager

**Files:** `src/logos_core/module_manager.h`, `src/logos_core/module_manager.cpp`

**Purpose:** Thin facade that orchestrates `ModuleRegistry`, `RuntimeRegistry`, and `DependencyResolver`. Provides the C++-level API for module lifecycle management. Each module runs in a separate subprocess managed by the selected `ModuleRuntime` implementation (default: `SubprocessManager`, which spawns `logos_host_qt` processes).

**Thread safety:** `loadModule`, `loadModuleWithDependencies`, and `unloadModule` are serialised by a static `loadMutex()` (one load/unload at a time). `discoverInstalledModules` delegates to `ModuleRegistry` which has its own reader-writer lock.

**API (namespace `ModuleManager`):**

| Method | Description |
|--------|-------------|
| `registry() → ModuleRegistry&` | Access the shared module registry |
| `runtimes() → RuntimeRegistry&` | Access the shared runtime registry |
| `setModulesDir(path)` | Set the primary module directory (clears existing) |
| `addModulesDir(path)` | Add an additional module directory |
| `setPersistenceBasePath(path)` | Set base directory for module instance persistence |
| `setModuleTransports(name, json)` | Register a per-module `LogosTransportSet` (serialized JSON). Threaded through to the child subprocess on load so its provider binds every listener instead of only the global default. Empty clears the entry. Read+write protected by `loadMutex()` |
| `discoverInstalledModules()` | Scan all module directories and register discovered modules |
| `processModule(path) → std::string` | Extract metadata from a module file, register as known |
| `processModuleCStr(path) → char*` | C-string variant of processModule |
| `loadModule(name) → bool` | Load a module (selects a runtime via RuntimeRegistry, spawns subprocess, sends auth token) |
| `loadModuleWithDependencies(name) → bool` | Resolve dependency tree, load in topological order |
| `initializeCapabilityModule() → bool` | Load the built-in capability module if available |
| `unloadModule(name) → bool` | Terminate module process and update registry |
| `unloadModuleWithDependents(name) → bool` | Cascade unload: terminate the named module together with every currently loaded module that transitively depends on it, leaves-first |
| `terminateAll()` | Terminate all running module processes |
| `clear()` | Clear registry and reset all state |
| `resolveDependencies(modules) → std::vector<std::string>` | Topological sort with circular dependency detection |
| `getDependencies(name, recursive) → std::vector<std::string>` | Declared dependencies of `name` among known modules; walks the forward graph transitively when `recursive=true`. Cycle- and diamond-safe BFS |
| `getDependents(name, recursive) → std::vector<std::string>` | Declared dependents of `name` among known modules; walks the reverse graph transitively when `recursive=true`. Reads from the in-process registry, no disk query |
| `getDependenciesCStr(name, recursive) → char**` | C-string variant backing `logos_core_get_module_dependencies` |
| `getDependentsCStr(name, recursive) → char**` | C-string variant backing `logos_core_get_module_dependents` |
| `getLoadedModulesCStr() → char**` | Return loaded module names as null-terminated C string array |
| `getKnownModulesCStr() → char**` | Return known module names as null-terminated C string array |
| `isModuleLoaded(name) → bool` | Check if a module is currently loaded |
| `getModuleProcessIds() → std::unordered_map<std::string, int64_t>` | Return module name → process ID mappings |

### ModuleRegistry

**Files:** `src/logos_core/module_registry.h`, `src/logos_core/module_registry.cpp`

**Purpose:** In-memory registry of discovered and loaded modules. Single source of truth for the dependency graph: stores module paths, forward dependencies, and the derived reverse edges (dependents). All public methods are thread-safe: mutating methods acquire a `std::unique_lock` on an internal `std::shared_mutex`; read-only methods acquire a `std::shared_lock`, allowing concurrent reads.

**Data:**
- `ModuleInfo` struct — holds `path`, `dependencies` (`std::vector<std::string>`), `dependents` (`std::vector<std::string>`, reverse-edge cache), `loaded` flag, `runtime` (`std::shared_ptr<ModuleRuntime>`), `handle` (`LoadedModuleHandle`)
- `std::unordered_map<std::string, ModuleInfo> m_modules` — module database keyed by name
- `std::vector<std::string> m_modulesDirs` — configured module directories
- `std::shared_mutex m_mutex` — reader-writer lock protecting all fields

**Dependency graph invariant:** `ModuleInfo::dependents` mirrors the inverse of `dependencies` across all known modules. `ModuleRegistry` owns this invariant and maintains it by calling the private `recomputeDependentsLocked()` at the tail of every forward-edge mutation (`discoverInstalledModules`, `processModule`, `registerModule` when deps are passed, `registerDependencies`). Callers never populate `dependents` directly. This replaces the previous pattern of querying `PackageManagerLib::resolveDependents()` on disk — the registry is now the single authority for reverse-dep lookups, and `ModuleManager::getDependents` / `unloadModuleWithDependents` read straight from it.

**API (class `ModuleRegistry`):**

| Method | Description |
|--------|-------------|
| `setModulesDir(dir)` | Clear and set single module directory |
| `addModulesDir(dir)` | Add to module directory list |
| `modulesDirs() → std::vector<std::string>` | Return configured directories |
| `discoverInstalledModules()` | Scan directories, parse manifest.json files; recomputes dependents at end |
| `processModule(path) → std::string` | Extract metadata from module file, register as known; recomputes dependents at end |
| `registerModule(name, path, deps)` | Manually register a module; recomputes dependents when deps are passed |
| `registerDependencies(name, deps)` | Set dependencies for a known module; recomputes dependents |
| `isKnown(name) → bool` | Module exists in registry |
| `modulePath(name) → std::string` | Get file path for a known module |
| `moduleDependencies(name, recursive) → std::vector<std::string>` | Forward-edge lookup. `recursive=false` returns direct dependencies from `ModuleInfo`; `recursive=true` walks the forward graph breadth-first (cycle/diamond safe) |
| `moduleDependents(name, recursive) → std::vector<std::string>` | Reverse-edge lookup. `recursive=false` returns direct dependents from `ModuleInfo`; `recursive=true` walks the reverse graph breadth-first (cycle/diamond safe) |
| `knownModuleNames() → std::vector<std::string>` | All discovered module names |
| `isLoaded(name) → bool` | Module is currently running |
| `markLoaded(name)` / `markUnloaded(name)` | Update load state |
| `markLoaded(name, runtime, handle)` | Mark loaded with associated runtime and handle |
| `runtimeFor(name) → std::shared_ptr<ModuleRuntime>` | Get the runtime that loaded a given module |
| `loadedModuleNames() → std::vector<std::string>` | Currently running module names |
| `clearLoaded()` | Clear all loaded state |
| `clear()` | Reset entire registry |

### ModuleRuntime (interface)

**Files:** `src/logos_core/module_runtime.h`

**Purpose:** Abstract interface for module loading strategies. Decouples the core from any specific subprocess or module-loading mechanism (Qt, WASM, in-process, etc.). Each implementation handles a particular module format.

**Supporting types:**
- `ModuleDescriptor` — describes a module to load: `name`, `path`, `format`, `modulesDirs`, `instancePersistencePath`, `transportSetJson` (per-module `LogosTransportSet` serialized as JSON; empty = inherit the global default LocalSocket), `onTerminated` callback
- `LoadedModuleHandle` — opaque handle returned by `load()`: `pid` and `runtimeData` (`std::any`)

**API (class `ModuleRuntime`):**

| Method | Description |
|--------|-------------|
| `id() → std::string` | Unique runtime identifier (e.g. `"qt-subprocess"`) |
| `canHandle(desc) → bool` | Whether this runtime can load the given module descriptor |
| `load(desc) → std::optional<LoadedModuleHandle>` | Load a module, return a handle on success |
| `sendToken(handle, token) → bool` | Send auth token to the loaded module |
| `terminate(handle)` | Terminate a specific loaded module |
| `terminateAll()` | Terminate all modules managed by this runtime |
| `getAllPids() → std::unordered_map<std::string, int64_t>` | Map module names to process IDs |

### RuntimeRegistry

**Files:** `src/logos_core/runtime_registry.h`, `src/logos_core/runtime_registry.cpp`

**Purpose:** Central registry of `ModuleRuntime` implementations. Selects the appropriate runtime for a given `ModuleDescriptor` by iterating registered runtimes and calling `canHandle()`. Fans out `terminateAll()` and `getAllPids()` across all registered runtimes.

**API (class `RuntimeRegistry`):**

| Method | Description |
|--------|-------------|
| `add(runtime)` | Register a `ModuleRuntime` implementation |
| `select(desc) → std::shared_ptr<ModuleRuntime>` | Find the first runtime that can handle the descriptor |
| `terminateAll()` | Terminate all modules across all runtimes |
| `getAllPids() → std::unordered_map<std::string, int64_t>` | Aggregate PIDs from all runtimes |

### DependencyResolver

**Files:** `src/logos_core/dependency_resolver.h`, `src/logos_core/dependency_resolver.cpp`

**Purpose:** Compute topological sort of module dependencies using Kahn's algorithm. Detects circular dependencies and missing modules.

**API (namespace `DependencyResolver`):**

| Method | Description |
|--------|-------------|
| `resolve(requested, isKnown, getDependencies) → std::vector<std::string>` | Returns modules in load order (dependencies first) |

Takes callback functions (`IsKnownFn`, `GetDependenciesFn`) so it has no coupling to the registry implementation.

### SubprocessManager

**Files:** `src/runtimes/runtime_qt/subprocess_manager.h`, `src/runtimes/runtime_qt/subprocess_manager.cpp`

**Purpose:** Concrete `ModuleRuntime` implementation for the Qt subprocess strategy, and the low-level subprocess management layer using Boost.Process v2 and Boost.Asio. This is the **default runtime** — it handles modules with `format == "qt-plugin"` as well as modules with an empty format field. `id()` returns `"qt-subprocess"`.

- Resolves and spawns `logos_host_qt` processes per module
- Uses `boost::process::v2::process` for subprocess spawning and `boost::asio::io_context` for async I/O
- Background `io_context` thread with work guard for non-blocking async read and wait callbacks
- Async read loop for stdout/stderr with line buffering
- Synchronous kill with graceful SIGTERM → SIGKILL escalation (5s timeout)
- Unix domain socket for token delivery (matches previous `QLocalSocket` behavior)
- A `std::mutex` (`s_processesMutex`) protects the `s_processes` map against concurrent access
- Shared pointer-based lifetime management for safe async callback handling

**ModuleRuntime interface:** `id()`, `canHandle()`, `load()`, `sendToken()`, `terminate()`, `terminateAll()`, `hasModule()`, `pid()`, `getAllPids()`

**Static process management API (used by tests):**

| Method | Description |
|--------|-------------|
| `startProcess(name, executable, arguments, callbacks) → bool` | Launch a subprocess with async output monitoring |
| `sendTokenToProcess(name, token) → bool` | Send auth token via Unix domain socket |
| `terminateProcess(name)` | Gracefully terminate a specific process |
| `terminateAllProcesses()` | Terminate all managed processes |
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

**Files:** `src/runtimes/runtime_qt/logos_host.cpp`, `src/runtimes/runtime_qt/command_line_parser.h/cpp`, `src/runtimes/runtime_qt/module_initializer.h/cpp`, `src/runtimes/runtime_qt/qt/qt_app.h/cpp`, `src/runtimes/runtime_qt/qt/qt_token_receiver.h/cpp`

**Purpose:** Lightweight subprocess (`logos_host_qt`) that loads a single Qt module. Parses `--name`, `--path`, optional `--instance-persistence-path`, and optional `--transport-set` (per-module `LogosTransportSet` JSON; empty = global-default LocalSocket only) arguments, loads the module, authenticates via token from the core, constructs the `LogosAPI` with the parsed transport set (explicit-transport ctor when provided, single-arg ctor otherwise), registers the module with the remote object registry, and runs the Qt event loop. A `logos_host` compatibility symlink is installed for backward compatibility with downstream consumers.

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

**Module Management:**

| Function | Description |
|----------|-------------|
| `logos_core_set_modules_dir(dir)` | Set primary module directory |
| `logos_core_add_modules_dir(dir)` | Add additional module directory |
| `logos_core_set_persistence_base_path(path)` | Set base directory for module instance persistence |
| `logos_core_set_module_transports(name, json)` | Register a per-module transport set (JSON, see logos-cpp-sdk shape). Forwarded to the child via `--transport-set` so its `LogosAPIProvider` binds every listener instead of only the global default LocalSocket. Must be called before the module is loaded; empty clears the entry |
| `logos_core_load_module(name) → int` | Load a module (1 = success, 0 = failure) |
| `logos_core_load_module_with_dependencies(name) → int` | Load module and dependencies in order |
| `logos_core_unload_module(name) → int` | Unload a module |
| `logos_core_unload_module_with_dependents(name) → int` | Cascade unload: terminate the module together with every loaded transitive dependent, leaves-first. Returns 1 only if every step succeeded |
| `logos_core_get_module_dependencies(name, recursive) → char**` | Modules that `name` depends on (forward edges). `recursive=true` walks the forward graph transitively. Unknown names yield an empty array. Caller frees |
| `logos_core_get_module_dependents(name, recursive) → char**` | Modules that depend on `name` (reverse edges). `recursive=true` walks transitively. Unknown names yield an empty array. Caller frees |
| `logos_core_process_module(path) → char*` | Process module file, return name (caller frees) |
| `logos_core_refresh_modules()` | Re-scan module directories |

**Queries:**

| Function | Description |
|----------|-------------|
| `logos_core_get_loaded_modules() → char**` | Null-terminated array of loaded names (caller frees) |
| `logos_core_get_known_modules() → char**` | Null-terminated array of known names (caller frees) |
| `logos_core_get_module_stats() → char*` | JSON array of CPU/memory stats (caller frees) |
| `logos_core_get_token(key) → char*` | Get auth token by key (caller frees) |

### Thread Safety

| Category | Guarantee |
|----------|-----------|
| `logos_core_load_module`, `logos_core_load_module_with_dependencies`, `logos_core_unload_module`, `logos_core_unload_module_with_dependents` | Serialised by a single internal mutex — safe to call concurrently from multiple threads. The cascade variant holds the lock for the entire leaves-first teardown so a late-arriving load can't interleave between tearing down the dependents and the target |
| `logos_core_get_known_modules`, `logos_core_get_loaded_modules` | Protected by a shared reader-writer lock — safe to call concurrently with each other and with the mutating functions above |
| `logos_core_refresh_modules` | Protected by `ModuleRegistry`'s reader-writer lock (write side) — safe for concurrent registry access but not serialised against load/unload |
| `logos_core_init`, `logos_core_start`, `logos_core_cleanup` | Not thread-safe — must be called from a single thread during startup/shutdown |

## Build Artifacts

| Artifact | Description |
|----------|-------------|
| `liblogos_core.{so,dylib,dll}` | Core shared library (C API) |
| `logos_host_qt` | Qt module subprocess host binary |
| `logos_host` | Compatibility symlink → `logos_host_qt` |
| `logos_core_tests` | Google Test suite |

## Operational

### Nix (Recommended)

Nix provides reproducible builds with all dependencies managed automatically.

**Build everything (binaries + libraries + headers):**
```bash
nix build
```

The result includes:
- `result/bin/logos_host_qt` — Qt module host binary
- `result/bin/logos_host` — Compatibility symlink → `logos_host_qt`
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
- OpenSSL (transitive: SDK's plain-C++ TCP+TLS transport)
- CLI11
- Google Test (fetched via FetchContent if not system-installed)

**Build:**
```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

The `logos_host_qt` binary will be in `build/bin/` and `liblogos_core` in `build/lib/`.

**Run tests:**
```bash
make -j$(nproc)
ctest --output-on-failure
```

**Note:** CMake expects the logos-cpp-sdk and logos-module libraries to be available. For a pre-built SDK it uses `find_package(logos-cpp-sdk)` (the package config carries OpenSSL / Boost / nlohmann_json / Qt as transitive deps); otherwise it falls back to the source tree (`cpp/CMakeLists.txt`).

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
    logos_core_set_modules_dir("/path/to/modules");

    logos_core_start();
    logos_core_load_module("chat");

    char** loaded = logos_core_get_loaded_modules();
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
logos_core_load_module_with_dependencies("my_module");
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
