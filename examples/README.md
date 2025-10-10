# logos-liblogos examples

This directory contains a minimal C CLI that links against `liblogos_core` to verify the library works and to print the known and loaded plugins.
The example embeds an rpath so the executable can locate `liblogos_core.dylib` in `../result/lib` at runtime so this assumes you have built the project with Nix already.

## Build

```bash
cd logos-liblogos/examples
make
```

## Run

By default, the example will use the library's default plugin discovery path. You can override the plugins directory using `LOGOS_PLUGINS_DIR`.

```bash
# Optionally set a custom plugins directory
export LOGOS_PLUGINS_DIR="$(pwd)/../bin/plugins"

# Run the CLI
./cli
```

If the library and discovery work, you should see output similar to:

```
logos-cli: starting...
Known plugins:
  - package_manager
Loaded plugins:
  (none)
logos-cli: done.
```

## Notes

- The example embeds an rpath so the executable can locate `liblogos_core.dylib` in `../result/lib` at runtime.
- Arrays returned by the C API are managed by the library; this short-lived example does not free them.
