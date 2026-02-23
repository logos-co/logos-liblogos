# logos-liblogos

## How to Build

### Using Nix (Recommended)

The project includes a Nix flake for reproducible builds with a modular structure:

#### Build Complete Library (Binaries + Libraries + Headers)

```bash
# Build everything (default)
nix build

# Or explicitly
nix build '.#logos-liblogos'
nix build '.#default'
```

The result will include:
- `/bin/` - Core binaries (logoscore, logos_host)
- `/lib/` - Core libraries (liblogos_core, liblogoscore, liblogos_host)
- `/include/` - Headers (interface.h)

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
```

#### Running Tests

**Using Nix (Recommended):**

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

**Manual Build:**

```bash
# From the build directory
cd build-local
ninja logos_core_tests

# Run tests
./bin/logos_core_tests

# Or use CTest
ctest --output-on-failure
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
- `nix/bin.nix` - Extracts binaries (includes libraries for runtime linking)
- `nix/lib.nix` - Extracts libraries only
- `nix/include.nix` - Header installation
- `nix/tests.nix` - Test suite build and execution

**Note:** The `logos-liblogos-bin` package includes both binaries and their required libraries to ensure proper runtime linking. This is necessary because the binaries dynamically link against the logos libraries.

#### Local Development

To use a local `logos-cpp-sdk` repo:

```bash
nix build --override-input logos-cpp-sdk path:../logos-cpp-sdk
```

### Manual Build

First, initialize the git submodules:

```bash
git submodule update --init --recursive
```

Then build:

```bash
./scripts/compile.sh
```

The built libraries will be available in `build/lib/` and binaries in `build/bin/`.

## Usage

### logoscore Command

The `logoscore` binary is the main entry point for the Logos Core application framework.

#### Command Line Options

- `--modules-dir <path>`, `-m <path>` - Specify a custom directory to scan for modules. If not provided, defaults to `../modules` relative to the application binary.

- `--load-modules <modules>`, `-l <modules>` - Comma-separated list of modules to load in order. Modules are loaded after the application starts. note: if not modules are left out they will automatically be loaded if there is a dependency specified between them. for example `--load-modules logos_irc` is the same as `--load-modules waku_module,chat,logos_irc`

- `--call <call>`, `-c <call>` - Call a module method: `module.method(param1, param2)`. Use `@file` to read a parameter from a file. Can be repeated multiple times to execute calls sequentially. Calls execute synchronously and abort on error.

- `--help`, `-h` - Display help information and available options.

- `--version` - Display version information.

#### Examples

```bash
# Run with default modules directory
./result/bin/logoscore

# Run with a custom modules directory
./result/bin/logoscore --modules-dir /path/to/custom/modules
# Or using the short form
./result/bin/logoscore -m /path/to/custom/modules

# Load specific modules
./result/bin/logoscore --load-modules module1,module2,module3
# Or using the short form
./result/bin/logoscore -l module1,module2,module3

# Combine options: custom modules directory and load specific modules
./result/bin/logoscore -m /path/to/modules -l module1,module2

# Call a module method with no parameters
./result/bin/logoscore -l my_module --call "my_module.start()"
# Or using the short form
./result/bin/logoscore -l my_module -c "my_module.start()"

# Call a module method with parameters
./result/bin/logoscore -l storage --call "storage.init('config', 42, true)"

# Read a parameter from a file (use @ prefix)
./result/bin/logoscore -l storage --call "storage.loadConfig(@config.json)"

# Multiple sequential calls
./result/bin/logoscore -l storage --call "storage.init(@config.txt)" --call "storage.start()"

# Combine all options: load modules and call methods
./result/bin/logoscore -m ./modules -l storage,chat \
  --call "storage.init(@config.json)" \
  --call "chat.connect('localhost', 8080)"

# Display help
./result/bin/logoscore --help
```

### Requirements

#### Build Tools
- CMake (3.14 or later)
- Ninja build system
- pkg-config

#### Dependencies
- Qt6 (qtbase) or Qt5 (Core, RemoteObjects)
- Qt6 Remote Objects (qtremoteobjects)
- [logos-cpp-sdk](https://github.com/logos-co/logos-cpp-sdk)

## Supported Platforms
- macOS (aarch64-darwin, x86_64-darwin)
- Linux (aarch64-linux, x86_64-linux)

## Disclaimer
This repository is part of an experimental development environment. Components are under active development and may be incomplete, unstable, modified or discontinued at any time.

The software is provided for development and testing purposes only and is not intended for production use. 

The code and related materials are made available on an open-source, “as-is” basis without warranties or guarantees of any kind, express or implied, including warranties of correctness, security, performance or fitness for a particular purpose. Use at your own risk.
