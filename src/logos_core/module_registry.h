#ifndef MODULE_REGISTRY_H
#define MODULE_REGISTRY_H

#include "module_runtime.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>

struct ModuleInfo {
    std::string path;
    std::vector<std::string> dependencies;
    // Direct reverse edges — names of modules whose `dependencies` list
    // includes this module. Kept in sync with `dependencies` across every
    // graph mutation by ModuleRegistry itself; callers never populate it
    // directly. Use ModuleRegistry::moduleDependents() for transitive walks.
    std::vector<std::string> dependents;
    bool loaded = false;
    // Null when loaded directly via markLoaded(name) (test/external scenarios).
    std::shared_ptr<LogosCore::ModuleRuntime> runtime;
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

    // Full mark-as-loaded that stores the owning runtime and handle for later
    // use by unloadModule(). Should be called from loadModuleInternal().
    void markLoaded(const std::string& name,
                    std::shared_ptr<LogosCore::ModuleRuntime> runtime,
                    LogosCore::LoadedModuleHandle handle);

    void markUnloaded(const std::string& name);
    std::vector<std::string> loadedModuleNames() const;
    void clearLoaded();

    // Returns the runtime that owns the named loaded module, or nullptr if
    // loaded without a runtime association (e.g. via markLoaded(name) only).
    std::shared_ptr<LogosCore::ModuleRuntime> runtimeFor(const std::string& name) const;

    void clear();

private:
    std::string processModuleInternal(const std::string& modulePath);

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
