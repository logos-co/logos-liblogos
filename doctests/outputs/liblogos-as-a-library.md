# Embedding This liblogos as a Library

# WARNING
  ** IMPORTANT **
  This doctest is for internal dev purposes and should not be used. It displays important flaws that will be corrected, specifically that QCoreApplication should no longer be needed but it's pending some changes still at which point this doctest will be updated to reflect that
  ** IMPORTANT **
`logos-liblogos` is a **library**. The headless `logoscore` CLI and the
basecamp desktop app are just frontends for it — each one links
`liblogos_core` and drives its C API (`logos_core.h`) to discover, load, and
manage modules. This doc-test shows how to do the same thing in **your own
program**, built against **this** liblogos commit:

1. Build `liblogos_core` (the C-API shared library), its `logos_host`
   subprocess binary, and the `logos_core.h` header — all from the commit
   under test.
2. Build the `lgpm` package manager and install two real modules
   ([`capability_module`](https://github.com/logos-co/logos-capability-module)
   and [`test_basic_module`](https://github.com/logos-co/logos-test-modules))
   into a local modules directory.
3. Write a ~60-line C++ program that links `liblogos_core`, starts the
   runtime with `capability_module` as its token authority, and loads
   `test_basic_module` through `core_service` over its shell binding.
4. Build that program against this liblogos and run it — watching the module
   come up over liblogos' own IPC.

Because the library, the host binary, and the headers all come from the commit
under test, a green run is direct evidence that **this** liblogos is usable as
an embeddable library, not just through the `logoscore` frontend.

> **Why C++ and not pure C?** The API in `logos_core.h` is plain C, but the
> runtime is built on Qt internally (it uses `QPluginLoader` to read module
> metadata and Qt's IPC stack to talk to modules). So an embedding program must
> construct a `QCoreApplication` before it calls `logos_core_start()` — exactly
> as `logoscore` and basecamp do. You do **not** need to run Qt's event loop;
> the load calls are synchronous.

**What you'll build:** A standalone C++ program that links `liblogos_core` from this commit and loads a real module — `test_basic_module` — through the C API.

**What you'll learn:**

- How to build `liblogos_core`, `logos_host`, and `logos_core.h` from a specific liblogos commit
- What a minimal `CMakeLists.txt` that links `liblogos_core` looks like
- The lifecycle of the C API: `logos_core_init` → `set_bundled_modules_dirs` → `set_shell_identity` → `start` → `take_shell_binding` → core_service calls → `cleanup`
- Why an embedding program needs a `QCoreApplication`, and where `logos_host` fits in (`LOGOS_HOST_PATH`)
- How to install modules with `lgpm` so the runtime can discover them

## Prerequisites

- **Nix** with flakes enabled. Install from [nixos.org](https://nixos.org/download.html), then enable flakes:

```bash
mkdir -p ~/.config/nix
echo 'experimental-features = nix-command flakes' >> ~/.config/nix/nix.conf
```

Verify: `nix flake --help >/dev/null 2>&1 && echo "Flakes enabled"`

- **A Linux or macOS machine.** The program runs headless via `QT_QPA_PLATFORM=offscreen`, so no display is required.

---

## How embedding liblogos works

Your program links `liblogos_core` and calls its C API. The runtime loads
each module in an isolated subprocess that it launches through a separate
`logos_host` binary, and talks to it over the Logos IPC bridge:

Everything below comes from the liblogos commit under test: the
`liblogos_core` you link, the `logos_host` that hosts the module, and the
`logos_core.h` header you compile against.

## Step 1: Build liblogos from this commit

Build the combined `logos-liblogos` package and symlink it to `./liblogos`.
The result contains everything an embedder needs: the C-API library, the
header, and the `logos_host` binary.

> The URL carries a release placeholder that pins the build to a specific
> commit: the doc-test runner expands it to a concrete ref. Locally that is
> this checkout's `HEAD` (see `run.sh`); in CI it is the commit being
> tested. With no pin it falls back to the latest `master`.

### 1.1 Build the library

```bash
nix build 'github:logos-co/logos-liblogos' -o ./liblogos
```

The result has three things your program depends on:

- `liblogos/include/logos_core.h` — the C API you compile against
- `liblogos/lib/liblogos_core.dylib` — the shared library you link
- `liblogos/bin/logos_host` — the subprocess host the runtime launches
  to run each module

### 1.2 Confirm the library, header, and host binary are present

```bash
ls liblogos/include/logos_core.h liblogos/lib/liblogos_core.dylib liblogos/bin/logos_host
```

---

## Step 2: Build lgpm and the modules to load

The runtime discovers modules from a directory of installed packages. We
build the `lgpm` package manager and two real modules' `.lgx` packages,
then install them in the next step. `capability_module` is the runtime's
token authority: nothing loads without it, so we install it alongside
`test_basic_module`, and the program will name that directory as its
bundled modules so the runtime runs it in-process.

### 2.1 Build lgpm

```bash
nix build 'github:logos-co/logos-package-manager#cli' -o lgpm
```

The executable is at `./lgpm/bin/lgpm`.

### 2.2 Build the capability module's .lgx

```bash
nix build 'github:logos-co/logos-capability-module#lgx' -o cap-lgx
```

The `.lgx` package is under `./cap-lgx/`:

```bash
ls cap-lgx/*.lgx
```

### 2.3 Build the test module's .lgx

`logos-test-modules` publishes each of its modules under
`modules.<system>.<name>`, so the attribute path carries the Nix system
double — `builtins.currentSystem` supplies it.

```bash
SYSTEM=$(nix eval --impure --raw --expr 'builtins.currentSystem')
nix build "github:logos-co/logos-test-modules#modules.$SYSTEM.test_basic_module.lgx" -o module-lgx

```

The `.lgx` package is under `./module-lgx/`:

```bash
ls module-lgx/*.lgx
```

---

## Step 3: Install the modules with lgpm

Install both packages into a local `./modules` directory. Installing through
`lgpm` writes each module's `manifest.json` with `"type": "core"`, which is
what makes the runtime discover them at startup. Both are unsigned local dev
builds, so we pass `--allow-unsigned`.

### 3.1 Install capability_module

```bash
./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file cap-lgx/*.lgx
```

### 3.2 Install test_basic_module

```bash
./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file module-lgx/*.lgx
```

### 3.3 Confirm both modules are installed

```bash
./lgpm/bin/lgpm --modules-dir ./modules list
```

Both modules now report type `core`, so `logos_core_start()` will
discover them.

---

## Step 4: Write the embedding program

Now the program itself. It is plain C++ that links `liblogos_core` and
drives the C API. Two files: the source and a `CMakeLists.txt`.

### 4.1 src/main.cpp

The whole lifecycle in one `main()`:

- Construct a `QCoreApplication` (liblogos uses Qt internally), but
  never call `app.exec()` — the calls are synchronous.
- Resolve the modules directory to an absolute path (`logos_core`
  cannot read plugin metadata from a relative path).
- `logos_core_init` → `logos_core_set_bundled_modules_dirs` →
  `logos_core_set_shell_identity` → `logos_core_start` (this discovers
  the modules and brings up `capability_module` in-process).
- `logos_core_take_shell_binding` hands the program its own identity.
  Every call about modules goes through it to `core_service`:
  `loadModule(name, "required")` loads the module and its
  dependencies, and `listModules` says what is up.

```cpp
// Embedding the Logos runtime (liblogos_core) in your own program.
//
// liblogos exposes a C API in <logos_core.h>: configuration before
// start, the runtime's lifecycle, and a shell binding. Everything
// about modules is a call to core_service, the runtime's control
// surface, made through that binding as this program's own identity.
// capability_module, the token authority, must be in a bundled
// directory: the runtime runs it in-process, and without it nothing
// loads. Internally the runtime is built on Qt, so the program
// constructs a QCoreApplication; it never runs Qt's event loop.
// Module subprocesses are launched through a separate `logos_host`
// binary, located via the LOGOS_HOST_PATH environment variable.
#include <QCoreApplication>
#include "logos_core.h"

#include <cstdio>
#include <filesystem>
#include <string>

// core_service's answer, as JSON text.
static std::string call(logos_consumer* shell, const char* method,
                        const std::string& args) {
    char* result = nullptr;
    char* error = nullptr;
    const int status = logos_consumer_call(shell, "core_service", method,
                                           args.c_str(), 120000, &result, &error);
    std::string text = status == 0 && result ? result
                     : std::string("(no answer) ") + (error ? error : "");
    logos_consumer_string_free(result);
    logos_consumer_string_free(error);
    return text;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    const char* modulesArg = (argc > 1) ? argv[1] : "./modules";
    const char* moduleName = (argc > 2) ? argv[2] : "test_basic_module";

    // logos_core needs an absolute modules directory: it cannot read
    // plugin metadata from a relative path. Resolve it up front.
    const std::string modulesDir =
        std::filesystem::absolute(modulesArg).string();

    QCoreApplication app(argc, argv);

    printf("Initializing Logos runtime...\n");
    logos_core_init(argc, argv);
    // This program ships its modules here, capability_module among them.
    const char* bundled[] = {modulesDir.c_str(), nullptr};
    logos_core_set_bundled_modules_dirs(bundled);
    logos_core_set_shell_identity("embed_app");

    printf("Starting (modules dir: %s)\n", modulesDir.c_str());
    logos_core_start();
    logos_consumer* shell = logos_core_take_shell_binding();
    if (!shell) {
        printf("No shell binding: capability_module is not running\n");
        logos_core_cleanup();
        return 1;
    }
    printf("Discovered modules: %s\n", call(shell, "listModules", R"(["all"])").c_str());

    printf("Loading '%s' (with dependencies)...\n", moduleName);
    const std::string loaded =
        call(shell, "loadModule", std::string("[\"") + moduleName + "\",\"required\"]");
    const bool ok = loaded.find("\"status\":\"ok\"") != std::string::npos;
    printf("Load %s\n", ok ? "OK" : "FAILED");
    printf("Loaded modules: %s\n", call(shell, "listModules", R"(["loaded"])").c_str());

    logos_consumer_release(shell);
    logos_core_cleanup();
    return ok ? 0 : 1;
}
```

### 4.2 CMakeLists.txt

A minimal build: find Qt6 Core, find `liblogos_core` under
`LOGOS_LIBLOGOS_ROOT` (the `nix build` result), and link both. The
`BUILD_RPATH` lets the program find `liblogos_core.dylib` at runtime
without setting `LD_LIBRARY_PATH`.

```cmake
cmake_minimum_required(VERSION 3.16)
project(logos_embed_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# liblogos is built on Qt; an embedding program needs Qt Core too.
find_package(Qt6 REQUIRED COMPONENTS Core)

# Point this at the `nix build` result of logos-liblogos
# (it has bin/, lib/, and include/). Defaults to ../liblogos.
if(NOT DEFINED LOGOS_LIBLOGOS_ROOT)
    set(LOGOS_LIBLOGOS_ROOT "${CMAKE_SOURCE_DIR}/../liblogos")
endif()
get_filename_component(LOGOS_LIBLOGOS_ROOT "${LOGOS_LIBLOGOS_ROOT}" ABSOLUTE)

find_library(LOGOS_CORE_LIBRARY NAMES logos_core
    PATHS "${LOGOS_LIBLOGOS_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)

add_executable(logos_embed_app src/main.cpp)
target_include_directories(logos_embed_app PRIVATE
    "${LOGOS_LIBLOGOS_ROOT}/include")
target_link_libraries(logos_embed_app PRIVATE
    ${LOGOS_CORE_LIBRARY} Qt6::Core)

# Find liblogos_core.{so,dylib} at runtime without LD_LIBRARY_PATH.
set_target_properties(logos_embed_app PROPERTIES
    BUILD_RPATH "${LOGOS_LIBLOGOS_ROOT}/lib")
```

---

## Step 5: Build the program against this liblogos

Build the program with CMake. We run the build **inside this liblogos'
dev shell** (`nix develop`) so the compiler and Qt version exactly match
the library you are linking — mismatched Qt is the most common cause of
embedding failures.

### 5.1 Configure and build

```bash
# Enter the liblogos dev shell (cmake, ninja, pkg-config, Qt6), then:
cd embed-app
cmake -S . -B build -G Ninja -DLOGOS_LIBLOGOS_ROOT="$PWD/../liblogos"
cmake --build build
```

The program is at `embed-app/build/logos_embed_app`. Its `BUILD_RPATH`
points at `liblogos/lib`, so it runs without `LD_LIBRARY_PATH`.

---

## Step 6: Run it against the real module

Run the program. It needs two things in the environment:

- `LOGOS_HOST_PATH` — the `logos_host` binary the runtime launches per
  module (here, the one from the library we built).
- `QT_QPA_PLATFORM=offscreen` — so Qt needs no display.

We point it at `./modules` and ask it to load `test_basic_module`.

### 6.1 Load test_basic_module through core_service

```bash
export LOGOS_HOST_PATH="$PWD/liblogos/bin/logos_host"
export QT_QPA_PLATFORM=offscreen
./embed-app/build/logos_embed_app ./modules test_basic_module
```

The output shows the runtime starting, bringing up `capability_module`
in-process as the token authority, then loading `test_basic_module` (a
real subprocess launched via `logos_host` and wired up over liblogos'
IPC) through `core_service`. `listModules` answers JSON records, one
per module.

That is your own program — not the `logoscore` CLI — running a real
module on top of this liblogos.

---

## Step 7: Inspect the module's API

`core_service` loads and manages modules; calling their methods goes over
the typed IPC bridge, which the frontends wrap. To see what
`test_basic_module` exposes, introspect the installed plugin with `lm`, the
module inspector from [`logos-module`](https://github.com/logos-co/logos-module).

### 7.1 Build lm

```bash
nix build 'github:logos-co/logos-module#lm' -o lm
```

### 7.2 List the module's invokable methods

```bash
lm methods ./modules/test_basic_module/test_basic_module_plugin.dylib
```

These are the `Q_INVOKABLE` methods the module you just loaded exposes —
the entry points a frontend would call over the IPC bridge once the
module is up.
