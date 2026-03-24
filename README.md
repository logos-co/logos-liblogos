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

// Plugin management
void logos_core_add_plugins_dir(const char* dir);
int  logos_core_load_plugin(const char* name);
int  logos_core_load_plugin_with_dependencies(const char* name);

// Async callbacks
typedef void (*AsyncCallback)(int result, const char* message, void* user_data);
void logos_core_call_plugin_method_async(
    const char* plugin_name, const char* method_name,
    const char* params_json, AsyncCallback callback, void* user_data);
```

See `src/logos_core/logos_core.h` for the full API.

## Dev vs Portable Builds

The library supports two build modes controlled by the `LOGOS_PORTABLE_BUILD` CMake flag:

- **Dev build** (default): Plugin loading looks for LGX variants with `-dev` suffix (e.g., `linux-amd64-dev`). Used in Nix/development environments.
- **Portable build** (`-DLOGOS_PORTABLE_BUILD=ON`): Looks for portable variants without suffix (e.g., `linux-amd64`). Used in self-contained distributed applications.

Build the portable variant with `nix build '.#portable'`.

## Supported Platforms

- macOS (aarch64-darwin, x86_64-darwin)
- Linux (aarch64-linux, x86_64-linux)
