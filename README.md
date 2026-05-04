# logos-liblogos

The core runtime library for the Logos modular application platform. Provides `liblogos_core` (a C-API shared library) and `logos_host` (the module subprocess host binary).

`logos-liblogos` is a **library**. It is consumed by two frontends:
- **[logos-basecamp](https://github.com/logos-co/logos-basecamp)** — the desktop GUI application shell
- **[logos-logoscore-cli](https://github.com/logos-co/logos-logoscore-cli)** — the headless CLI runtime (`logoscore`)

## How to Build

The project uses a Nix flake for reproducible builds with a modular structure:

#### Build Complete Library (Binaries + Libraries + Headers)

```bash
# Build everything (default)
nix build

# Or explicitly
nix build '.#logos-liblogos'
nix build '.#default'
```

The result will include:
- `/bin/` - Host binary (logos_host)
- `/lib/` - Core library (liblogos_core)
- `/include/` - Headers (logos_core.h, interface.h)

#### Build Individual Components

```bash
# Build only the binaries (outputs to /bin)
nix build '.#logos-liblogos-bin'

# Build only the libraries (outputs to /lib)
nix build '.#logos-liblogos-lib'

# Build only the headers (outputs to /include)
nix build '.#logos-liblogos-include'

# Build and run tests
nix build '.#logos-liblogos-tests'

# Build portable variant (selects portable LGX variants instead of dev)
nix build '.#portable'
```

#### Running Tests

```bash
# Build and run tests (tests run automatically during build)
nix build '.#logos-liblogos-tests'

# To run tests manually after building:
./result/bin/logos_core_tests

# Run specific tests
./result/bin/logos_core_tests --gtest_filter=AppLifecycleTest.*

# List all available tests
./result/bin/logos_core_tests --gtest_list_tests
```

#### Development Shell

```bash
# Enter development shell with all dependencies
nix develop
```

**Note:** In zsh, you need to quote targets with `#` to prevent glob expansion.

If you don't have flakes enabled globally, add experimental flags:

```bash
nix build '.#logos-liblogos' --extra-experimental-features 'nix-command flakes'
```

The compiled artifacts can be found at `result/`

#### Modular Architecture

The nix build system is organized into modular files in the `/nix` directory:
- `nix/default.nix` - Common configuration (dependencies, flags, metadata)
- `nix/build.nix` - Shared build that compiles everything once
- `nix/bin.nix` - Extracts binaries (logos_host, includes libraries for runtime linking)
- `nix/lib.nix` - Extracts libraries only
- `nix/include.nix` - Header installation
- `nix/tests.nix` - Test suite build and execution

**Note:** The `logos-liblogos-bin` package includes both the `logos_host` binary and its required libraries to ensure proper runtime linking.

#### Local Development

To use a local `logos-cpp-sdk` repo:

```bash
nix build --override-input logos-cpp-sdk path:../logos-cpp-sdk
```

## Library API

`logos-liblogos` exposes a C API via `logos_core.h`:

```c
// Lifecycle
void logos_core_init(int argc, char *argv[]);
void logos_core_start();
int  logos_core_exec();
void logos_core_cleanup();

// Module directory management
void logos_core_set_modules_dir(const char* dir);
void logos_core_add_modules_dir(const char* dir);

// Instance persistence
void logos_core_set_persistence_base_path(const char* path);

// Per-module transport configuration (forwarded to the module's
// child subprocess so its LogosAPIProvider binds every listener
// instead of only the global default LocalSocket). Must be called
// before the module is loaded.
void logos_core_set_module_transports(const char* name, const char* transport_set_json);

// Module management
int  logos_core_load_module(const char* name);
int  logos_core_load_module_with_dependencies(const char* name);
int  logos_core_unload_module(const char* name);
int  logos_core_unload_module_with_dependents(const char* name);
char* logos_core_process_module(const char* path);
void logos_core_refresh_modules();

// Dependency graph queries (forward + reverse edges; recursive walks BFS)
char** logos_core_get_module_dependencies(const char* name, bool recursive);
char** logos_core_get_module_dependents(const char* name, bool recursive);

// Module queries
char** logos_core_get_loaded_modules();
char** logos_core_get_known_modules();

// Module stats and tokens
char* logos_core_get_module_stats();
char* logos_core_get_token(const char* key);

// Event loop integration
void logos_core_process_events();
```

See `src/logos_core/logos_core.h` for the full API.

### Thread safety

Module load/unload operations (`logos_core_load_module`, `logos_core_load_module_with_dependencies`, `logos_core_unload_module`, `logos_core_unload_module_with_dependents`) are serialised internally by a single mutex. It is safe to call them concurrently from multiple threads, including rapid and repeated load/unload cycles on the same module — each call waits for its turn and the process management layer handles teardown cleanly before the next launch. `logos_core_unload_module_with_dependents` in particular holds the lock for its entire leaves-first teardown so a late-arriving load can't interleave between tearing down a dependent and its parent.

`logos_core_refresh_modules` is synchronised through the module registry's reader-writer lock — it is safe to call concurrently with other registry accesses, but it is **not** serialised against load/unload by the same mutex as above.

Read-only accessors (`logos_core_get_known_modules`, `logos_core_get_loaded_modules`) use that shared reader-writer lock and are safe to call concurrently with each other and with `logos_core_refresh_modules`.

## Dev vs Portable Builds

The library supports two build modes controlled by the `LOGOS_PORTABLE_BUILD` CMake flag:

- **Dev build** (default): Module loading looks for LGX variants with `-dev` suffix (e.g., `linux-amd64-dev`). Used in Nix/development environments.
- **Portable build** (`-DLOGOS_PORTABLE_BUILD=ON`): Looks for portable variants without suffix (e.g., `linux-amd64`). Used in self-contained distributed applications.

Build the portable variant with `nix build '.#portable'`.

## Supported Platforms

- macOS (aarch64-darwin, x86_64-darwin)
- Linux (aarch64-linux, x86_64-linux)
