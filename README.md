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
