# logos-liblogos

## How to Build

### Using Nix (Recommended)

The project includes a Nix flake for reproducible builds:

```bash
# Build the library
nix build

# Enter development shell with all dependencies
nix develop
```

**Note:** In zsh, you need to quote targets with `#` to prevent glob expansion.

If you don't have flakes enabled globally, add experimental flags:

```bash
nix build --extra-experimental-features 'nix-command flakes'
```

The compiled library can be found at `result/`

The core binary can be found at `./result/bin/logoscore`

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
