# Plan: Pluggable Module Runtimes in `logos-liblogos`

This document describes how to abstract module loading and transport so `logos_core` orchestrates **modules** without hard-coding Qt plugins, subprocess + `logos_host`, or Qt Remote Objects over Unix sockets.

## 1. What is coupled today

The core currently hard-wires three concerns together:


| Concern                  | Where                                                                                                             | Coupling                                                                                                                                           |
| ------------------------ | ----------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Discovery / metadata** | `PluginRegistry` (`plugin_registry.cpp`)                                                                          | Uses `ModuleLib::LogosModule::getModuleName` / `getModuleDependencies`, which open the binary with `QPluginLoader` (Qt plugin format is baked in). |
| **Isolation strategy**   | `PluginLauncher::launch` → `QtProcessManager::startProcess`                                                       | Always spawns a `logos_host` subprocess via Boost.Process.                                                                                         |
| **Transport**            | `logos_host` (token receiver, `plugin_initializer` + `LogosAPIProvider`) + token delivery over Unix domain socket | Registry + Qt Remote Objects is implicit everywhere.                                                                                               |
| **Module ABI**           | `logos_host` calls `module.as<PluginInterface>()` and expects a `QObject` with `Q_INVOKABLE` methods              | Only Qt/C++ plugins work.                                                                                                                          |


`PluginManager::loadPluginInternal` performs a single linear recipe: generate token → `PluginLauncher::launch` → `sendToken` → `markLoaded` → notify capability module. That recipe should become polymorphic.

## 2. Proposed architecture: `ModuleRuntime` abstraction

Introduce a single extension point — a `**ModuleRuntime`** — and reduce `PluginManager` to a registry of runtimes plus orchestrator. Analogy: containerd → runc / kata / gvisor: the **core** decides **what** to load and **when**; the **runtime** decides **how**.

### 2.1 The interface

```cpp
// src/logos_core/module_runtime.h (conceptual)
namespace LogosCore {

struct ModuleDescriptor {
    std::string name;
    std::string path;                      // file or directory; meaning depends on runtime
    std::string format;                    // "qt-plugin" | "extism-wasm" | "native-lib" | ...
    std::vector<std::string> dependencies;
    std::string instancePersistencePath;
    std::vector<std::string> pluginsDirs;
    nlohmann::json rawMetadata;            // from manifest.json / embedded metadata
    nlohmann::json runtimeConfig;          // free-form, e.g. docker image, grpc endpoint
};

struct LoadedModuleHandle {
    std::string name;
    int64_t pid = -1;                      // -1 when not a process (in-proc, wasm, remote)
    std::string endpoint;                  // transport-specific: unix:///..., grpc://host:port, inproc://<id>
    std::any opaque;                       // runtime-private state
};

class ModuleRuntime {
public:
    virtual ~ModuleRuntime() = default;

    // Identity + capabilities
    virtual std::string id() const = 0;                 // "qt-subprocess", "extism", "inproc", "docker"
    virtual bool canHandle(const ModuleDescriptor&) const = 0;

    // Lifecycle (must be idempotent per name)
    virtual bool load(const ModuleDescriptor&,
                      std::function<void(const std::string&)> onTerminated,
                      LoadedModuleHandle& out) = 0;
    virtual bool sendToken(const std::string& name, const std::string& token) = 0;
    virtual void terminate(const std::string& name) = 0;
    virtual void terminateAll() = 0;

    // Optional introspection
    virtual std::optional<int64_t> pid(const std::string& name) const { return std::nullopt; }
    virtual std::string endpoint(const std::string& name) const { return {}; }
};

} // namespace LogosCore
```

### 2.2 The registry / selector

```cpp
// src/logos_core/runtime_registry.h (conceptual)
class RuntimeRegistry {
public:
    void registerRuntime(std::shared_ptr<ModuleRuntime>);
    // Selection priority: explicit override in manifest > format match > first canHandle
    std::shared_ptr<ModuleRuntime> select(const ModuleDescriptor&) const;

    // Load discovery of dynamic runtime plugins (e.g. liblogos-runtime-extism.so)
    void loadRuntimePlugin(const std::string& soPath);
};
```

Runtimes can be implemented as shared libraries with a single C entry point:

```cpp
extern "C" LOGOS_RUNTIME_EXPORT
LogosCore::ModuleRuntime* logos_runtime_create(const LogosCore::RuntimeHostServices*);
```

`RuntimeHostServices` gives a runtime back-references it may need (e.g. `LogosAPI` for the `core` identity, `TokenManager`, logging, registry access — everything today’s `notifyCapabilityModule` uses).

### 2.3 `PluginManager` becomes transport-agnostic

`loadPluginInternal` collapses to:

1. Build `ModuleDescriptor` from registry (manifest + embedded metadata merged).
2. `auto runtime = runtimes.select(desc);` — fail if none.
3. `LoadedModuleHandle h;` — `runtime->load(desc, onTerminated, h)`.
4. Generate token; `runtime->sendToken(name, token)` — on failure, `runtime->terminate(name)` and return false.
5. `registry.markLoaded(name, h)` — registry stores handle + owning runtime.
6. `TokenManager::instance().saveToken(name, token);`
7. `notifyCapabilityModule(name, token);`

`PluginRegistry` keeps both the runtime reference and `LoadedModuleHandle` per loaded module so `unloadPlugin`, `getPluginProcessIds`, and stats routing know who owns what.

### 2.4 Mapping requested variants to runtimes


| Variant                                                          | Runtime impl                                                                                                                                                                                                                      | Notes                                                                                                                       |
| ---------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| **Status quo** — `logos_host` subprocess + QtRO over Unix socket | `QtSubprocessRuntime` (wraps today’s `PluginLauncher` + `QtProcessManager`)                                                                                                                                                       | No behavioral change; default for `format: "qt-plugin"`.                                                                    |
| **In-process**                                                   | `InProcRuntime` — `dlopen` + instantiate `PluginInterface` in the host, register with local `LogosAPIProvider` using **local transport** (SDK already has `implementations/qt_local/local_transport.`*).                          | `pid = getpid()`, `endpoint = "inproc://<module>"`. Token delivery can short-circuit via `TokenManager` inside `sendToken`. |
| **WASM (Extism)**                                                | `ExtismRuntime` — loads `.wasm`, drives via host functions; shim exposes exports so the rest of the core sees a consistent invocation surface (bridge implementing whatever abstract `LogosObject`-style API you standardize on). | No QObject. Metadata from sidecar `manifest.json`, not only `QPluginLoader`.                                                |
| **Different transport (gRPC)**                                   | `GrpcSubprocessRuntime` — spawns a host binary (could be `logos_host` built with gRPC transport, or language-specific host), reads `endpoint` from stdout or config, registers with SDK transport.                                | SDK already has `LogosTransport` (`qt_remote`, `qt_local`, `mock`); add `grpc` and wire from runtime.                       |
| **Docker**                                                       | `DockerRuntime` — `docker run` (or libdocker) with image from `runtimeConfig`, mount token socket or expose port; container ID as logical “pid” or map via `docker inspect` for stats.                                            | Same `ModuleRuntime` interface; different `load` implementation.                                                            |


### 2.5 How a module picks its runtime

Two mechanisms, layered:

1. **Per-module override** in `manifest.json` (authoritative example):
  ```json
   {
     "name": "my_module",
     "runtime": {
       "id": "docker",
       "config": { "image": "ghcr.io/me/my_module:1.2.3", "transport": "grpc" }
     }
   }
  ```
2. **Format sniff** when `runtime` is absent: file extension (`.wasm` → extism, `.so`/`.dylib` with Qt plugin metadata → qt-subprocess), then first runtime whose `canHandle` returns true.

A **core-wide policy** (env var or `logos_core_set_default_runtime`) can force e.g. “everything in-process” for tests or “everything in docker” in production — expressed as an ordered list of runtime IDs consulted before `canHandle`.

### 2.6 Where today’s code moves


| Today                                                          | Tomorrow                                                                                                                                                |
| -------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `plugin_launcher.{h,cpp}`                                      | Logic moves into `runtimes/qt_subprocess/qt_subprocess_runtime.{h,cpp}` (or equivalent path).                                                           |
| `process_manager.{h,cpp}` (`QtProcessManager` namespace)       | Rename to something like `SubprocessManager`; keep as shared utility for any runtime that spawns OS processes (qt-subprocess, docker, grpc subprocess). |
| `logos_host/*`                                                 | Stays; owned conceptually by the qt-subprocess runtime. Other runtimes ship their own host binary (or none).                                            |
| `plugin_manager.cpp` — `notifyCapabilityModule`, orchestration | Stays in `PluginManager`; no runtime-specific branching beyond `select` + virtual calls.                                                                |
| `PluginRegistry` + `ModuleLib::LogosModule::getModuleName`     | Pluggable manifest reader and/or Qt metadata as **fallback**; primary source can become `manifest.json` fields including `format` / `runtime`.          |


### 2.7 Capability module / token flow

`notifyCapabilityModule` can stay largely as-is: it already uses `LogosAPI` / `LogosAPIClient`, which sit on the SDK’s transport abstraction. As long as each runtime registers the module with the same provider model the capability module expects, inter-module auth keeps working. **The SDK already abstracted transport; `logos-liblogos` is the main place still assuming “subprocess + QtRO”.**

## 3. Suggested rollout (green at every step)

1. **Introduce `ModuleRuntime` + `RuntimeRegistry`**, ship `QtSubprocessRuntime` as a thin wrapper around today’s `PluginLauncher` / process manager. Wire `PluginManager` through it only. **No behavior change.**
2. **Teach `PluginRegistry` to read `manifest.json` as primary** (if applicable to your package layout) and keep Qt-plugin metadata as fallback; add `runtime` / `format` fields (ignored until non-default runtimes exist).
3. **Add `InProcRuntime`** behind manifest opt-in or env override — validates second runtime + local transport; good for tests and trusted built-ins.
4. **Rename `QtProcessManager` → `SubprocessManager`** (or similar) and document as shared subprocess utility.
5. **Add `ExtismRuntime`** as optional shared library (`liblogos-runtime-extism.so`) loaded via `RuntimeRegistry::loadRuntimePlugin` — keeps wasm/extism deps out of core.
6. **Add `GrpcSubprocessRuntime` + gRPC `LogosTransport` in logos-cpp-sdk** — proves transport swap end-to-end.
7. **Add `DockerRuntime`** once subprocess + optional SO loading paths are stable.

## 4. Risks and design notes

- **Token exchange is runtime-specific** (Unix socket today in `qt_token_receiver` / `QtProcessManager::sendToken`). Each runtime owns end-to-end token delivery; avoid forcing one mechanism on all.
- **Stats** (`ProcessStats::getModuleStats`) assumes PIDs. Widen to module handles: `(pid-or-0, endpoint, runtime_id)` so UI/ops can show in-process, container ID, etc.
- **Event loop.** Qt subprocess runtime relies on Qt in `logos_host`. In-proc needs a Qt event loop in the host process where applicable. The runtime contract should state which runtimes require a loop and whether the host must pump it.
- `**logos_core_exec()` is currently a no-op** in this repo — acceptable; runtimes that need a loop either use the embedding app’s loop or an internal thread. Avoid leaking “must pump events” into the C API unless necessary.
- **Keep `ModuleRuntime` headers Qt-free** so optional runtimes do not pull Qt into the abstraction layer.
- **Discovery namespace:** multiple formats can collide on `name`. Treat manifest `name` as authoritative; on-disk artifact is opaque to the selected runtime.

## 5. Minimal first commit (concrete starting point)

Zero behavior change, three conceptual additions:

1. `src/logos_core/module_runtime.h` — interface + `ModuleDescriptor` / `LoadedModuleHandle`.
2. `src/logos_core/runtime_registry.{h,cpp}` — register runtimes, `select(descriptor)`.
3. `src/logos_core/runtimes/qt_subprocess_runtime.{h,cpp}` — move current `PluginLauncher` logic here; `id()` returns `"qt-subprocess"`; `canHandle` true for `format == "qt-plugin"` or empty format (default).

Then change `plugin_manager.cpp` to call `runtime->load` / `sendToken` / `terminate` instead of `PluginLauncher::`* directly. Downstream (`logos_host`, `PluginRegistry`, C API, tests) unchanged. Every further runtime is additive.

## 6. Goals recap

- `**logos_core` abstracts** discovery, load order, tokens, capability notification, unload — not **how** a module is hosted.
- **Extensions** (WASM, in-proc, gRPC, Docker) ship as runtimes (in-tree or `.so`), selected by manifest and/or policy.
- **Reuse** existing SDK transport split (`qt_remote`, `qt_local`, mock) when adding gRPC or other transports instead of forking the whole stack.

