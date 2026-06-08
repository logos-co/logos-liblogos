# Logos Core Library

## Overall Description

Logos Core is a modular runtime platform for hosting and orchestrating independently developed modules. It provides a C-API shared library (`liblogos_core`) and a pluggable runtime system that together enable a module-based architecture for decentralised applications. The runtime system separates two orthogonal concerns — **containers** (how a module process is launched, managed, and communicates) and **module loaders** (how a specific type of module binary is resolved and loaded). These are composed into a `CompositeRuntime` that implements the `ModuleRuntime` interface. The default configuration pairs a `SubprocessContainer` (Boost.Process v2) with a `QtPluginRuntime` (Qt plugin loader) to spawn a `logos_host_qt` process per module.

The platform is designed to:
- Load, start, stop, and introspect modules at runtime
- Isolate each module in its own process for robustness and security
- Provide transparent inter-module RPC via a remote object registry
- Support token-based authentication for secure module-to-module communication
- Expose a C API so that host applications in any language can drive the runtime
- Support diverse container strategies (subprocess, Docker, in-process) and module formats (Qt plugin, WASM) through composable abstractions

## Definitions & Acronyms

| Term | Definition |
|------|------------|
| **Module** | An independently developed module that implements `PluginInterface` and is dynamically loaded by the core |
| **Core Library** | `liblogos_core` — the shared library that provides the C API for module management |
| **Module Host** | `logos_host_qt` — a lightweight executable that loads a single Qt module in its own process (a `logos_host` symlink exists for backward compatibility) |
| **Module Container** | An abstract interface (`ModuleContainer`) that defines how a module's execution environment is managed — process lifecycle, I/O, and credential delivery (e.g. subprocess, Docker, in-process) |
| **Module Loader** | An abstract interface (`ModuleLoader`) that defines how a specific type of module binary is resolved and prepared for loading — host binary resolution and CLI argument construction (e.g. Qt plugin, WASM) |
| **Module Runtime** | An abstract interface (`ModuleRuntime`) that encapsulates a complete strategy for loading and managing modules. Implemented by `CompositeRuntime`, which pairs a `ModuleContainer` with a `ModuleLoader` |
| **Composite Runtime** | A concrete `ModuleRuntime` that composes a `ModuleContainer` and a `ModuleLoader`, delegating process lifecycle to the container and module-type resolution to the loader |
| **Runtime Registry** | The central registry (`RuntimeRegistry`) that selects the appropriate `ModuleRuntime` for a given module based on its format descriptor |
| **Core Manager** | A built-in module that exposes core functionality via RPC, allowing modules to manage the core without linking against the C API |
| **Capability Module** | A built-in module that handles authorization tokens for inter-module communication |
| **RPC** | Remote Procedure Call — the mechanism by which modules invoke methods on each other |
| **IPC** | Inter-Process Communication — the underlying transport |
| **Token** | A UUID-based authentication credential issued by the core or capability module for securing RPC calls |
| **SDK** | The [logos-cpp-sdk](https://github.com/logos-co/logos-cpp-sdk) — client library that abstracts connection management, token handling, and asynchronous invocation |

## Domain Model

### System Architecture

At a high level, the Logos Core consists of:

**Core Library** — The C/C++ shared library (`liblogos_core`) that provides the API functions for lifecycle management, module loading/unloading, and introspection.

**Runtime Registry** — A central registry of `ModuleRuntime` implementations. When a module is loaded, the registry selects the first runtime whose `canHandle()` returns true for the module's descriptor. This decouples the core from any specific loading mechanism.

**Module Containers** — Pluggable implementations of the `ModuleContainer` interface that define the execution environment for modules. The default `SubprocessContainer` spawns a separate process per module using Boost.Process v2, manages I/O pipes, and handles token delivery over Unix sockets. Future containers could include Docker, in-process, or sandboxed environments.

**Module Loaders** — Pluggable implementations of the `ModuleLoader` interface that define how a specific type of module binary is prepared for loading. The default `QtPluginRuntime` resolves the `logos_host_qt` binary and constructs the appropriate CLI arguments. Future loaders could handle WASM (Extism), native shared libraries, or scripting runtimes.

**Composite Runtime** — A `ModuleRuntime` implementation that composes a `ModuleContainer` with a `ModuleLoader`. Its `load()` method first asks the loader to resolve the host binary and build arguments, then delegates process launch to the container. All other operations (sendToken, terminate, hasModule, pid) are forwarded to the container. The default composite runtime pairs `SubprocessContainer` with `QtPluginRuntime`.

**Core Manager** — A built-in module that runs in the core process and exposes core functionality as RPC methods, allowing remote modules to manage the core without linking against the C API directly.

**Module Host** — A lightweight executable (`logos_host`) that loads a single module in its own process. On startup it first receives an authentication token from the container (via Unix socket), then loads the Qt plugin and initializes `LogosAPI` with that token. This separation ensures container concerns (credential delivery) are handled independently from runtime concerns (plugin loading).

**Capability Module** — A built-in module that handles authorization for inter-module communication by issuing tokens and notifying both communicating parties.

**Remote Object Registry** — A registry that maintains a mapping of module names to remote object replicas and forwards method calls/events.

### Process Architecture

Each module runs in its own process for isolation:

```
┌──────────────────────────────────────────────────────┐
│  Host Application                                    │
│  ┌────────────────────────────────────────────────┐  │
│  │  liblogos_core                                 │  │
│  │  ├─ RuntimeRegistry                            │  │
│  │  │   └─ CompositeRuntime (default)              │  │
│  │  │       ├─ SubprocessContainer (container)     │  │
│  │  │       └─ QtPluginRuntime (loader)            │  │
│  │  ├─ Core Manager (built-in module)             │  │
│  │  ├─ Capability Module (built-in module)        │  │
│  │  └─ Remote Object Registry                     │  │
│  └────────────────────────────────────────────────┘  │
│           │ IPC (local socket)                        │
│     ┌─────┼─────────┐                                │
│     ▼     ▼         ▼                                 │
│  ┌─────┐ ┌─────┐ ┌─────┐                             │
│  │host │ │host │ │host │  (logos_host_qt processes)   │
│  │mod A│ │mod B│ │mod C│                               │
│  └─────┘ └─────┘ └─────┘                             │
└──────────────────────────────────────────────────────┘
```

- The core uses a `RuntimeRegistry` to select the appropriate `ModuleRuntime` for each module
- The default `CompositeRuntime` pairs a `SubprocessContainer` with a `QtPluginRuntime`
- `SubprocessContainer` manages process lifecycle (spawn, terminate, token delivery) using Boost.Process v2
- `QtPluginRuntime` resolves the `logos_host_qt` binary and builds CLI arguments for Qt plugin modules
- Modules with `format == "qt-plugin"` or no explicit format are handled by the default composite runtime
- Communication happens via the Logos API. Each module's transport set is configured per-module by the host: by default modules listen on a LocalSocket only, but the host can register a `LogosTransportSet` (LocalSocket, TCP, TCP+TLS) per module via `logos_core_set_module_transports` and the loader threads it through to the child via `--transport-set`
- Faulty or untrusted modules cannot crash the core or other modules
- Modules can be written in different languages as long as they implement the RPC protocol
- Alternative containers (Docker, in-process) and loaders (WASM, Extism) can be composed and registered

### Token-Based Authentication

Since the remote object registry has no built-in security mechanisms, all RPC calls require an authentication token. This is transparent to module developers when using the SDK:

1. **Core → Module**: When a module is loaded, the core generates a UUID token and sends it to the module process via the container's `sendToken()` mechanism (currently a Unix domain socket managed by `SubprocessContainer`). The socket name is `logos_token_<module-name>` (scoped by `LOGOS_INSTANCE_ID` when set); both sides validate the module name as a single safe path segment before deriving the socket path, so an untrusted name cannot turn `removeServer()`/`listen()` into a file-unlink/clobber primitive (see Discovery, above). On the child side, `SubprocessTokenReceiver` receives the token before the module loader initializes the plugin. The module uses this token to authenticate calls from the core.

   **Listener authentication (CWE-940).** The token socket lives at a predictable path (`$TMPDIR/logos_token_<name>[_<instanceId>]`) in a world-writable temp directory, so a local co-tenant could pre-bind that path before the legitimate child calls `listen()`. A bare `connect()` only proves *something* is listening — not that it is the child the core spawned. Before writing the token, the parent therefore authenticates the connected peer's kernel-reported credentials: it requires the peer to run as the core's own effective uid, and — when the child's pid is known (the normal load path, where `launch()` records it before `sendToken()` runs) — to be exactly that process. The check uses `SO_PEERCRED` on Linux (uid + pid) and, on macOS, `getpeereid()` for the uid plus `getsockopt(SOL_LOCAL, LOCAL_PEERPID)` for the pid — so both platforms enforce the uid + pid gate. Any mismatch is fatal: the parent refuses to write the token rather than risk leaking it to a squatter, and the send fails. **Residual risk:** when the child pid is unknown (the placeholder path used by some callers) only the uid gate applies, so a same-uid co-tenant could still receive the token; and even with the pid gate a same-uid attacker can in principle race for the path between the child's `listen()` and the parent's `connect()`. Closing that window entirely requires handing the child a pre-connected `socketpair()` fd instead of a named path, eliminating the predictable socket altogether — a planned hardening. The child/receiver side (`SubprocessTokenReceiver`) has a symmetric exposure — it accepts the connecting peer without a credential check — and needs the mirror-image hardening; the `socketpair()` redesign would close both halves at once.
2. **Module → Module**: When modules need to communicate, they request authorization from the Capability Module, which issues a token and notifies both parties. The modules then use this token for subsequent requests.
3. **Token Storage**: Each module stores tokens in a thread-safe `TokenManager` (part of the SDK). `ModuleProxy` validates tokens before dispatching method calls.

#### Token-handoff socket hardening

The Core → Module handoff socket lives at a public, predictable path under the world-writable temp directory (`$TMPDIR`, default `/tmp`), so the local IPC channel is hardened against a same-host attacker injecting or intercepting the auth token.

**Threat (F-012).** On a shared/multi-user host, an attacker under a different uid knows a module (e.g. `chat_module`) is about to load — module names are not secret — and races the parent by repeatedly connecting to the predictable socket path `/tmp/logos_token_chat_module`. If it wins the connection, it sends a token `T` of its choosing; the child accepts `T` verbatim as its auth credential. The attacker then presents `T` on inter-module calls and is authorized as that module, bypassing the capability/token gate.

The hardening closes this on both sides of the handoff:

- **Owner-only socket node.** The child binds the `QLocalServer` with `UserAccessOption`, creating the socket node with mode `0600`. A process running under a different uid cannot `connect()` to it, closing the window where a co-tenant could race the parent and hand the child an attacker-chosen token.
- **Mutual peer-uid authentication.** Both sides verify the other end's uid before trusting it, via a Qt-free helper (`socketPeerIsSameUid()`, `SO_PEERCRED` on Linux / `getpeereuid()` on BSD/macOS). The child rejects any connecting peer whose uid is not its own; the parent refuses to write the token to a listener it did not authenticate as the same uid (guarding against a hostile listener planted at the predictable path). The check fails closed — a bad fd or syscall error is treated as untrusted.

This authenticates the connecting *uid*, not the specific parent/child process; a same-uid sibling could still race the socket. Closing that residual gap (a per-user `0700` socket directory and/or a pid check) is left as future hardening.

### Module Metadata

Every module ships a `metadata.json` referenced by Qt's `Q_PLUGIN_METADATA` macro. Required fields:

| Field | Purpose |
|-------|---------|
| `name` | Unique module identifier. **Enforced**, not advisory: it must match the installed package name at discovery time and the plugin's `name()` return value at load time — a mismatch on either is refused (see Discovery and Loading). |
| `version` | Semantic version string |
| `description` | Human-readable description |
| `author` | Module author or organization |
| `type` | Module type (e.g., `"core"`) |
| `category` | Module category for organization |
| `main` | Main module class name |
| `dependencies` | Array of required module names |
| `capabilities` | Array of capabilities this module provides |
| `include` | Optional array of extra files (shared libs, resources) to bundle |

## Features & Requirements

### Module Lifecycle

#### Discovery

1. Core scans configured module directories for `.so`, `.dylib`, or `.dll` files
2. For each file, metadata is extracted via `QPluginLoader`
3. A module's **identity is bound to the trusted package name** (the `manifest.json` name the package manager scanned and dedupes on), not to the name a plugin embeds in its own metadata. If a plugin's embedded `name` disagrees with its package name, the plugin is **refused** (not registered under either name). This prevents a package installed under an innocuous name from shipping a binary that claims a privileged identity (e.g.  `capability_module`) and inheriting that module's token/trust relationships.
4. Modules are added to the "known" list without being loaded
5. Multiple module directories can be configured

The module **name** comes from untrusted plugin JSON metadata and later becomes a filesystem/socket path segment (the per-module token socket `logos_token_<name>` and the instance-persistence directory). It is therefore validated at the trust
boundary: during processing (`ModuleRegistry::processModuleInternal`) a module whose name is not a single safe path segment — empty, `.`, `..`, longer than 255 bytes, or containing a path separator (`/`, `\`) or embedded NUL — is rejected and never added to the registry. The same check is applied as defense-in-depth at the token-socket sink in the host child (`SubprocessTokenReceiver::receive`, plus the `LOGOS_INSTANCE_ID` it appends), because a directly-invoked host re-derives the name from the `--name` CLI argument without going through the registry. This prevents a crafted name such as `seg/../victim` from escaping the socket/temp directory and driving `LocalServer::removeServer()` into unlinking an attacker-chosen file. See `logos::isSafePathSegment` (`src/path_safety.h`).

#### Loading

1. Core locates the module file for the requested module name
2. Core resolves dependencies and loads them first (topological sort with circular dependency detection)
3. If a persistence base path is configured, core resolves an instance ID and persistence directory for the module (reusing an existing instance or creating a new one)
4. Core builds a `ModuleDescriptor` (name, path, format, module dirs, persistence path, transport-set JSON if registered via `logos_core_set_module_transports`)
5. Core asks the `RuntimeRegistry` to `select()` a runtime for the descriptor (the default `CompositeRuntime` handles `"qt-plugin"` format and modules with no explicit format)
6. The selected runtime's `load()` is called:
   a. The `ModuleLoader` resolves the host binary (e.g. `logos_host_qt`) and builds CLI arguments (including `--transport-set` if configured)
   b. The `ModuleContainer` launches the process with the resolved binary and arguments
7. Core generates a UUID authentication token
8. Core sends the token to the module via the runtime's `sendToken()` (delegates to the container, which authenticates the receiving peer's credentials before writing the secret — see Token-Based Authentication)
9. Host process receives the token via `SubprocessTokenReceiver` (container concern), then loads the module plugin and calls `initLogos(LogosAPI*)` (loader/runtime concern). As a defense-in-depth identity check, the host **refuses to initialize** the plugin if its `name()` does not match the name it was loaded as (the trusted registry key passed by the core) — a binary cannot run, or receive tokens, under a name it does not implement
10. The `LogosAPI` instance exposes `modulePath`, `instanceId`, and `instancePersistencePath` as properties
11. Host process registers the module with the remote object registry
12. Core waits for registration and records the module as loaded (along with the runtime and handle)

#### Unloading

1. The module's host process is terminated
2. The module is removed from the loaded modules list
3. Associated tokens and state are cleaned up

#### Cascade Unloading

`logos_core_unload_module(name, true)` unloads the named module together with every currently loaded module that transitively depends on it. Teardown order is leaves-first (dependents before dependencies) so no process is left briefly pointing at a terminated parent. The call is serialised with ordinary load/unload operations under a single lock span — a late-arriving load cannot interleave between tearing down the dependents and the target.

### Dependency Resolution

- Dependencies are declared in each module's `metadata.json`
- `logos_core_load_module(name, true)` performs topological sort
- Circular dependencies are detected and cause the load to fail (returns 0)
- Missing/unknown dependencies cause the load to fail (returns 0)
- The resolver itself (`DependencyResolver::resolve`) returns a `ResolveResult` containing the partial topological order, a list of missing dependency names, and a cycle flag. The load path treats any resolution error as a hard failure; the teardown path (`unloadModuleWithDependents`) uses the partial order best-effort
- Dependencies are loaded in correct order before the requesting module
- The core maintains an in-process dependency graph with both forward and reverse edges. The reverse edges are re-derived from the forward edges at the tail of every discovery or metadata-processing pass, so cascade unload and dependent queries answer from memory without re-reading manifests from disk.

### Process Monitoring

- CPU percentage, CPU time, and memory usage tracked per module process
- Statistics returned as JSON via `logos_core_get_module_stats()`
- Core Manager process is excluded from stats
- Not available on iOS

### Thread Safety

The C API is designed to be safe for use from multi-threaded host applications:

- **Load/unload operations** (`load_module`, `unload_module`) are serialised — only one runs at a time, so rapid concurrent load/unload cycles on the same or different modules do not produce data races. The cascade variant (`with_dependents=true`) holds the lock for its full leaves-first teardown.
- **Read-only queries** (`get_known_modules`, `get_loaded_modules`) use a shared reader-writer lock and may execute concurrently with each other and with load/unload operations.
- **Module discovery** (`refresh_modules`) is protected by the registry's own write lock.
- **Lifecycle functions** (`init`, `start`, `cleanup`) are not thread-safe and must be called from a single thread.

### Dev vs Portable Builds

The platform supports two build variants:

- **Dev build** (default): Module loading looks for LGX variants with `-dev` suffix (e.g., `linux-amd64-dev`). Used in Nix/development environments.
- **Portable build**: Looks for portable variants without suffix (e.g., `linux-amd64`). Used in self-contained distributed applications.

## API Description

### Core Lifecycle

| Function | Purpose |
|----------|---------|
| `logos_core_init(argc, argv)` | Initialize global state, optionally set module directory. Creates a QCoreApplication if one does not exist. |
| `logos_core_add_modules_dir(path)` | Add a module directory to scan (duplicates ignored). |
| `logos_core_start()` | Scan module directories, process metadata, create Core Manager, load built-in modules, start remote object registry. |
| `logos_core_cleanup()` | Unload all modules, stop processes, clean up global state. |

### Module Management

| Function | Purpose |
|----------|---------|
| `logos_core_get_loaded_modules() → char**` | Return null-terminated array of loaded module names. Caller must free. |
| `logos_core_get_known_modules() → char**` | Return null-terminated array of all discovered modules. Caller must free. |
| `logos_core_load_module(name, with_dependencies) → int` | Load a module by name. When `with_dependencies` is true, resolves the dependency tree and loads in topological order. Returns 1 on success, 0 on failure. |
| `logos_core_unload_module(name, with_dependents) → int` | Terminate the module's process and remove it. When `with_dependents` is true, cascade unloads every loaded transitive dependent leaves-first. Returns 1 only if every step succeeded. |
| `logos_core_get_module_dependencies(name, recursive) → char**` | Return null-terminated array of modules that `name` depends on (forward edges). With `recursive=true`, walks the forward dependency graph transitively via BFS. Unknown names yield an empty array. Caller must free. |
| `logos_core_get_module_dependents(name, recursive) → char**` | Return null-terminated array of modules that depend on `name` (reverse edges). With `recursive=true`, walks the reverse dependency graph transitively via BFS. Unknown names yield an empty array. Caller must free. |
| `logos_core_process_module(path) → char*` | Read a module file's metadata and register it as known without loading. Returns the module name or NULL. Caller must free. |
| `logos_core_set_module_transports(name, json)` | Register a per-module `LogosTransportSet` (JSON, see logos-cpp-sdk shape) for the named module. The runtime forwards it to the child via `--transport-set` so the child's `LogosAPIProvider` binds every transport instead of only the global default LocalSocket. Must be called before the module is loaded. NULL or empty clears any previously-registered entry. |

### Token and Monitoring

| Function | Purpose |
|----------|---------|
| `logos_core_get_token(key) → char*` | Return the auth token for a key. Caller must free. NULL if not found. |
| `logos_core_get_module_stats() → char*` | Return JSON array of CPU/memory stats per loaded module. Caller must free. Not available on iOS. |

### Core Manager Module (RPC Surface)

The Core Manager is a built-in module exposing core functionality to remote modules:

| Method | Purpose |
|--------|---------|
| `setModulesDirectory(directory)` | Set the module search directory. |
| `start()` | Start the core's registry and load built-in modules. |
| `cleanup()` | Unload all modules and shut down. |
| `getLoadedModules() → std::vector<std::string>` | Return names of loaded modules. |
| `getKnownModules() → QJsonArray` | Return all known modules with `loaded` flag. |
| `loadModule(name) → bool` | Load a module by name. |
| `unloadModule(name) → bool` | Unload a module by name. |
| `processModule(filePath) → std::string` | Read a module file's metadata and register it. |
| `getModuleMethods(name) → QJsonArray` | Introspect a module's methods via Qt meta-object system. |

## Module Implementation

A complete module must implement the Logos Module interface.

### Inter-Module Communication

Modules communicate using the SDK's generated C++ wrappers:

```
logos-><module_name>.<method>(args...)       // call a remote method
logos-><module_name>.on("event", callback)   // subscribe to events
logos-><module_name>.trigger("event", data)  // emit an event
```

The SDK abstracts away registry lookup, token management, and async invocation.

## Supported Platforms

- macOS (aarch64-darwin, x86_64-darwin)
- Linux (aarch64-linux, x86_64-linux)

## Future Work

- **Signature support** — Signing and verifying module packages. Required to fully close the *Module Identity & Trust* residual: enforcing REQUIRE-mode signature checks for reserved/privileged module names so a malicious package cannot claim a privileged name (e.g. `capability_module`) by winning scan-time dedup from a user-writable directory
- **Additional containers** — Register alternative `ModuleContainer` implementations (e.g. Docker, in-process, sandboxed) that can be composed with any loader
- **Additional loaders** — Register alternative `ModuleLoader` implementations (e.g. WASM/Extism, native shared libraries) that can be composed with any container
- **Cross-language modules** — Modules in languages other than C++
- **Move away from Qt** — Logos API will move away from Qt. Process management has been migrated from Qt (`QProcess`) to Boost.Process v2, and Qt container/utility types (`QString`, `QStringList`, `QHash`, `QDir`, `QFile`, `QUuid`) have been replaced with standard C++ and Boost equivalents (`std::string`, `std::vector`, `std::unordered_map`, `std::filesystem`, `boost::uuids`). The container/loader separation (`ModuleContainer` / `ModuleLoader` / `CompositeRuntime` / `RuntimeRegistry`) decouples the core from any specific loading or execution strategy. Remaining Qt dependencies (event loop, module loading, remote objects) are isolated in `QtPluginRuntime`, `SubprocessTokenReceiver`, and the `logos_host_qt` binary.
