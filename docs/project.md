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
│   ├── common/
│   │   └── interface.h                  # PluginInterface definition (all modules must implement)
│   ├── logos_core/                      # Core library implementation
│   │   ├── logos_core.h                 # C API header
│   │   ├── logos_core.cpp               # C API implementation
│   │   ├── logos_core_internal.h        # Internal global state definitions
│   │   ├── app_lifecycle.h/cpp          # Application lifecycle management
│   │   ├── plugin_manager.h/cpp         # Module discovery, loading, dependency resolution
│   │   ├── proxy_api.h/cpp              # Async callback and event listener implementation
│   │   ├── process_stats.h/cpp          # CPU and memory monitoring per module
│   │   └── core_context.cpp             # Context management
│   └── logos_host/                      # Module subprocess host
│       ├── logos_host.cpp               # Host entry point
│       ├── command_line_parser.h/cpp    # CLI argument parsing (--name, --path)
│       └── plugin_initializer.h/cpp     # Plugin loading and token setup
├── tests/                               # Google Test suite
│   ├── CMakeLists.txt                   # Test build configuration
│   ├── test_app_lifecycle.cpp           # AppLifecycle tests
│   ├── test_plugin_manager.cpp          # PluginManager tests
│   ├── test_process_stats.cpp           # ProcessStats tests
│   └── test_proxy_api.cpp              # ProxyAPI async callback tests
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
| **C++11** | Implementation language |
| **CMake 3.14+** | Build system |
| **Logos API (c++ sdk)** | Event loop, plugin system, IPC, meta-object system |
| **zstd** | Compression (build dependency) |
| **Google Test** | Unit testing framework |
| **Nix** | Package management and reproducible builds |

### External Logos Dependencies

| Dependency | Purpose |
|------------|---------|
| **[logos-cpp-sdk](https://github.com/logos-co/logos-cpp-sdk)** | C++ client library (LogosAPI, LogosAPIProvider, LogosAPIClient, TokenManager) |
| **[logos-module](https://github.com/logos-co/logos-module)** | Base module library (metadata extraction, plugin loading utilities) |
| **[logos-capability-module](https://github.com/logos-co/logos-capability-module)** | Built-in capability authorization module |
| **[logos-nix](https://github.com/logos-co/logos-nix)** | Common Nix tooling and nixpkgs pin |

## Core Modules

### AppLifecycle

**Files:** `src/logos_core/app_lifecycle.h`, `src/logos_core/app_lifecycle.cpp`

**Purpose:** Application lifecycle management. Handles QCoreApplication creation, plugin directory configuration, startup, and cleanup.

**API (namespace `AppLifecycle`):**

| Method | Description |
|--------|-------------|
| `init(argc, argv)` | Initialize global state, create QCoreApplication if needed |
| `setPluginsDir(path)` | Set the primary plugin directory |
| `addPluginsDir(path)` | Add an additional plugin directory |
| `start()` | Scan plugins, create Core Manager, load built-in modules, start registry |
| `exec() → int` | Run the Qt event loop |
| `cleanup()` | Unload all modules, stop processes, clean up state |
| `processEvents()` | Process Qt events without blocking |

### PluginManager

**Files:** `src/logos_core/plugin_manager.h`, `src/logos_core/plugin_manager.cpp`

**Purpose:** Module discovery, loading, unloading, and dependency resolution. Each module runs in a separate `logos_host` process for isolation.

**API (namespace `PluginManager`):**

| Method | Description |
|--------|-------------|
| `processPlugin(path) → char*` | Extract metadata from a module file, register as known |
| `loadPlugin(name) → int` | Load a module (spawns `logos_host` process) |
| `loadPluginWithDependencies(name) → int` | Resolve dependency tree and load in correct order |
| `unloadPlugin(name) → int` | Terminate module process and remove from loaded list |
| `resolveDependencies(name) → list` | Topological sort with circular dependency detection |
| `getLoadedPlugins() → char**` | Return loaded module names |
| `getKnownPlugins() → char**` | Return all discovered module names |

### ProxyAPI

**note** These are experimental and used for experimental SDKs that currently cannot make direct calls to logos modules. As such these methods might be deprecated in the future.
__These methods are not used by any of the logos modules or apps.__

**Files:** `src/logos_core/proxy_api.h`, `src/logos_core/proxy_api.cpp`

**Purpose:** Async callback implementation for non-Qt consumers (e.g., Node.js/Electron). Provides async plugin loading, remote method invocation, and event listener registration.

**API (namespace `ProxyAPI`):**

| Method | Description |
|--------|-------------|
| `asyncOperation(data, callback, user_data)` | Example async helper for testing |
| `loadPluginAsync(name, callback, user_data)` | Asynchronously load a plugin |
| `callPluginMethodAsync(plugin, method, params_json, callback, user_data)` | Invoke a remote method with JSON parameters |
| `registerEventListener(plugin, event, callback, user_data)` | Subscribe to module events |

### ProcessStats

**Files:** `src/logos_core/process_stats.h`, `src/logos_core/process_stats.cpp`

**Purpose:** CPU and memory monitoring for loaded module processes.

**API (namespace `ProcessStats`):**

| Method | Description |
|--------|-------------|
| `getProcessStats(pid) → stats` | Return CPU %, CPU time, and memory usage for a process |
| `getModuleStats() → char*` | Return JSON array of stats for all loaded modules |

### LogosHost

**Files:** `src/logos_host/logos_host.cpp`, `src/logos_host/command_line_parser.h/cpp`, `src/logos_host/plugin_initializer.h/cpp`

**Purpose:** Lightweight subprocess that loads a single module. Parses `--name` and `--path` arguments, loads the plugin, authenticates via token from the core, registers the module with the remote object registry, and runs the Qt event loop.

### PluginInterface

**File:** `src/common/interface.h`

**Purpose:** Base interface that all modules must implement. Defines `name()`, `version()`, and `initLogos()` methods. Uses `Q_DECLARE_INTERFACE` for Qt's plugin system.

## Module Implementation

### Required Components

A complete module consists of:

1. **Interface Header** — Defines the module's public API contract. Must inherit from `PluginInterface`, mark public methods with `LOGOS_METHOD`, and include the `eventResponse` signal.
2. **Plugin Implementation** — Concrete class inheriting from `QObject` and the interface. Uses `Q_PLUGIN_METADATA` with IID and metadata file.
3. **Metadata File** — `metadata.json` describing module properties, dependencies, and capabilities.
4. **Build Configuration** — `CMakeLists.txt` producing a shared library (`.so`/`.dylib`/`.dll`).

### Interface Contract

All modules must:
- Inherit from `PluginInterface` (defined in `interface.h`)
- Implement `name()`, `version()`, and `initLogos(LogosAPI*)`
- Include `eventResponse(eventName, data)` signal for event forwarding
- Mark RPC-exposed methods with `LOGOS_METHOD`
- Use `Q_DECLARE_INTERFACE` macro for Qt's plugin system

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
- C++11 compatible compiler
- Qt 6 with Core and RemoteObjects modules
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

### Async Method Invocation (for Node.js/Electron)

**note** These are experimental and used for experimental SDKs that currently cannot make direct calls to logos modules. As such these methods might be deprecated in the future.
__These methods are not used by any of the logos modules or apps.__


```c
void on_result(int result, const char* message, void* user_data) {
    printf("Result: %d, Message: %s\n", result, message);
}

logos_core_load_plugin_async("chat", on_result, NULL);

logos_core_call_plugin_method_async(
    "chat", "sendMessage",
    "[{\"name\":\"channel\",\"value\":\"general\",\"type\":\"string\"}]",
    on_result, NULL
);

logos_core_register_event_listener("chat", "chatMessage", on_result, NULL);
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
