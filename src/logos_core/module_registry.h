#ifndef MODULE_REGISTRY_H
#define MODULE_REGISTRY_H

#include "module_loader.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>

namespace logos {

// Allowlist validator for an untrusted module name. The name comes verbatim
// from plugin metadata and becomes the registry map key, the LogosAPI RPC
// target, and the instance-persistence directory segment
// (basePath + "/" + name + "/" + instanceId). Rule: non-empty, <= 64 bytes,
// every byte in [A-Za-z0-9_-] — so '/', '\\', '.', "..", NUL and whitespace
// are all rejected, preventing path traversal (CWE-22) and key collision.
// Enforced at the trust boundary in ModuleRegistry::processModuleInternal.
bool isValidModuleName(const std::string& name);

}  // namespace logos

struct ModuleInfo {
    std::string path;
    // The module's full embedded metadata as a compact JSON string, read once
    // at discovery time via ModuleLib::LogosModule (no plugin instantiation).
    // Empty when the plugin exposes no readable metadata.
    std::string metadataJson;
    std::vector<std::string> dependencies;
    // Direct reverse edges — names of modules whose `dependencies` list
    // includes this module. Kept in sync with `dependencies` across every
    // graph mutation by ModuleRegistry itself; callers never populate it
    // directly. Use ModuleRegistry::moduleDependents() for transitive walks.
    std::vector<std::string> dependents;
    bool loaded = false;
    // Null when loaded directly via markLoaded(name) (test/external scenarios).
    std::shared_ptr<LogosCore::ModuleLoader> loader;
    LogosCore::LoadedModuleHandle handle;
};

class ModuleRegistry {
public:
    void setModulesDir(const std::string& dir);
    void addModulesDir(const std::string& dir);
    std::vector<std::string> modulesDirs() const;

    void discoverInstalledModules();
    std::string processModule(const std::string& modulePath);

    bool isKnown(const std::string& name) const;
    std::string modulePath(const std::string& name) const;
    // A JSON array describing every known module: one object per module with
    // its name, path, loaded flag, direct dependencies, direct dependents, and
    // full embedded metadata (parsed from the cached metadata JSON; null when
    // unreadable). This is the data backing logos_core_get_modules_info.
    nlohmann::json allModulesInfo() const;
    // Forward-edge accessor. `recursive=false` returns the direct
    // dependencies stored on ModuleInfo. `recursive=true` walks the forward
    // graph breadth-first and returns every transitive dependency. Unknown
    // names yield an empty list. Traversal is cycle- and diamond-safe.
    std::vector<std::string> moduleDependencies(const std::string& name,
                                                bool recursive = false) const;
    // Reverse-edge accessor. `recursive=false` returns the direct
    // dependents stored on ModuleInfo. `recursive=true` walks the reverse
    // graph breadth-first and returns every transitive dependent. Unknown
    // names yield an empty list.
    std::vector<std::string> moduleDependents(const std::string& name,
                                              bool recursive = false) const;
    std::vector<std::string> knownModuleNames() const;
    void registerModule(const std::string& name, const std::string& path,
                        const std::vector<std::string>& dependencies = {});
    void registerDependencies(const std::string& name, const std::vector<std::string>& dependencies);

    bool isLoaded(const std::string& name) const;
    void markLoaded(const std::string& name);

    // Full mark-as-loaded that stores the owning loader and handle for later
    // use by unloadModule(). Should be called from loadModuleInternal().
    void markLoaded(const std::string& name,
                    std::shared_ptr<LogosCore::ModuleLoader> loader,
                    LogosCore::LoadedModuleHandle handle);

    void markUnloaded(const std::string& name);
    std::vector<std::string> loadedModuleNames() const;
    void clearLoaded();

    // Returns the loader that owns the named loaded module, or nullptr if
    // loaded without a loader association (e.g. via markLoaded(name) only).
    std::shared_ptr<LogosCore::ModuleLoader> loaderFor(const std::string& name) const;

    void clear();

private:
    // Reads the plugin's metadata and upserts a ModuleInfo for it.
    //
    // `trustedName` binds the module's *identity*. When non-empty (the
    // discovery path, where it is the package-manager's InstalledPackage::name)
    // it is used as the registry key, and a plugin whose own embedded metadata
    // name disagrees is REFUSED — this is the F-022 guard against a binary
    // claiming a privileged name it doesn't legitimately own. When empty (the
    // raw processModule() host API), the embedded name is used as before.
    std::string processModuleInternal(const std::string& modulePath,
                                      const std::string& trustedName = {});

    // Re-derives every ModuleInfo::dependents list by inverting the
    // dependencies edges across m_modules. Called at the tail of
    // discoverInstalledModules() and processModule(), and by any other
    // mutation that can change the graph (including registerModule and
    // registerDependencies). Must be called with m_mutex held
    // exclusively. Cost is O(N * avg_deps) — negligible for the module
    // counts we see and simpler than keeping incremental diffs.
    void recomputeDependentsLocked();
    std::vector<std::string> moduleDependenciesLocked(const std::string& name,
                                                      bool recursive) const;
    std::vector<std::string> moduleDependentsLocked(const std::string& name,
                                                    bool recursive) const;

    mutable std::shared_mutex m_mutex;
    std::vector<std::string> m_modulesDirs;
    std::unordered_map<std::string, ModuleInfo> m_modules;
};

#endif // MODULE_REGISTRY_H
