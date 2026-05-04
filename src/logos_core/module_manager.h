#ifndef MODULE_MANAGER_H
#define MODULE_MANAGER_H

#include "runtime_registry.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

class ModuleRegistry;

namespace ModuleManager {
    ModuleRegistry& registry();

    // Access the runtime registry (e.g. for tests that need to install a FakeRuntime).
    LogosCore::RuntimeRegistry& runtimes();

    void setModulesDir(const char* modules_dir);
    void addModulesDir(const char* modules_dir);
    void setPersistenceBasePath(const char* path);

    // Register a per-module transport set (serialized JSON, see
    // logos-cpp-sdk/cpp/logos_transport_config_json.h for the wire
    // shape). The runtime threads this through to the child subprocess
    // when the module loads, so the child's LogosAPIProvider binds
    // every transport in the set instead of only the global default
    // (LocalSocket).
    //
    // Must be called BEFORE the module is loaded — by the daemon, this
    // means before logos_core_start() for capability_module, or before
    // any explicit loadModule() call for user modules. Unset modules
    // continue to use the global default.
    //
    // Empty `transportSetJson` clears any previously set entry.
    void setModuleTransports(const std::string& moduleName,
                             const std::string& transportSetJson);

    void discoverInstalledModules();

    std::string processModule(const std::string& modulePath);
    char* processModuleCStr(const char* modulePath);
    bool loadModule(const char* moduleName);
    bool loadModuleWithDependencies(const char* moduleName);
    bool initializeCapabilityModule();
    bool unloadModule(const char* moduleName);

    // Cascading unload: unload the named module together with every currently
    // loaded module that (transitively) depends on it. The order is
    // leaves-first (dependents before dependencies) so no process is left
    // briefly pointing at a dead parent.
    // Returns true only if every step succeeded.
    bool unloadModuleWithDependents(const char* moduleName);

    void terminateAll();
    void clear();

    char** getLoadedModulesCStr();
    char** getKnownModulesCStr();

    bool isModuleLoaded(const std::string& name);
    std::unordered_map<std::string, int64_t> getModuleProcessIds();

    std::vector<std::string> resolveDependencies(const std::vector<std::string>& requestedModules);

    // Returns the declared dependencies of `name` among known modules.
    // Names that appear only in module metadata and are not known to the
    // registry are not included in the returned list.
    // `recursive=true` walks the forward dependency graph transitively.
    std::vector<std::string> getDependencies(const std::string& name, bool recursive);

    // Returns the declared dependents of `name` among known modules.
    // Only known modules tracked by the registry are included in the
    // returned list.
    // `recursive=true` walks the reverse dependency graph transitively.
    std::vector<std::string> getDependents(const std::string& name, bool recursive);

    // Null-terminated char** variants of the two accessors above. Caller
    // owns the returned array and each entry. Pass-through of the registry
    // result — unknown names produce a zero-length (just a trailing null)
    // array, matching the C API contract used by the other getters.
    char** getDependenciesCStr(const char* name, bool recursive);
    char** getDependentsCStr(const char* name, bool recursive);
}

#endif // MODULE_MANAGER_H
