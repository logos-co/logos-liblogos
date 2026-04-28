#ifndef MODULE_RUNTIME_H
#define MODULE_RUNTIME_H

#include <any>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

// Qt-free abstract interface for module loading strategies.
// An implementation decides *how* a module is loaded, isolated, and communicated with.
// The core (ModuleManager) decides *what* to load and *when*.

namespace LogosCore {

// Describes a module that the core wants to load. Passed to ModuleRuntime::load().
struct ModuleDescriptor {
    std::string name;
    std::string path;                      // path to the module binary/bundle/wasm/etc.
    std::string format;                    // "qt-plugin", "wasm", "" (empty = default)
    std::vector<std::string> dependencies;
    std::string instancePersistencePath;   // empty if not configured
    std::vector<std::string> modulesDirs;  // directories siblings are looked up in
    nlohmann::json rawMetadata;            // metadata parsed from manifest.json
    nlohmann::json runtimeConfig;          // optional: {"id":"docker","image":"..."}, etc.

    // Per-module transport set, serialized as JSON (see
    // logos-cpp-sdk/cpp/logos_transport_config_json.h for the wire
    // shape). Empty = inherit the global default (LocalSocket only).
    // Threaded through to the child subprocess by ModuleRuntime
    // implementations so its LogosAPIProvider binds every transport
    // in the set rather than only the global default.
    std::string transportSetJson;
};

// A handle to a successfully loaded module. Stored in ModuleRegistry (ModuleInfo).
struct LoadedModuleHandle {
    std::string name;
    int64_t pid = -1;      // -1 when not process-based (in-proc, wasm, remote, etc.)
    std::string endpoint;  // transport-specific URI, e.g. "qtro+unix://my_module"
    std::any opaque;       // runtime-private state (optional)
};

// Abstract base: one instance per runtime kind, shared across all modules it manages.
// All implementations must be Qt-free at the interface level.
class ModuleRuntime {
public:
    virtual ~ModuleRuntime() = default;

    // Unique identifier for this runtime (e.g. "qt-subprocess", "inproc", "extism").
    virtual std::string id() const = 0;

    // Return true if this runtime knows how to load the described module.
    virtual bool canHandle(const ModuleDescriptor& desc) const = 0;

    // Load the module. On success, populate `out` and return true.
    // `onTerminated` may be called from a background thread when the module exits.
    virtual bool load(const ModuleDescriptor& desc,
                      std::function<void(const std::string& name)> onTerminated,
                      LoadedModuleHandle& out) = 0;

    // Deliver the auth token to the named module. Called immediately after a successful load().
    virtual bool sendToken(const std::string& name, const std::string& token) = 0;

    // Terminate a single module by name.
    virtual void terminate(const std::string& name) = 0;

    // Terminate all modules managed by this runtime.
    virtual void terminateAll() = 0;

    // Return true if this runtime currently has an active entry for the named module.
    virtual bool hasModule(const std::string& name) const = 0;

    // Return the PID of the named module, or nullopt if not process-based.
    virtual std::optional<int64_t> pid(const std::string& /*name*/) const { return std::nullopt; }

    // Return all (name -> pid) mappings. PIDs are -1 for non-process runtimes.
    virtual std::unordered_map<std::string, int64_t> getAllPids() const { return {}; }
};

} // namespace LogosCore

#endif // MODULE_RUNTIME_H
