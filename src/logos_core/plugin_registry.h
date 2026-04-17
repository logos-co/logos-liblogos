#ifndef PLUGIN_REGISTRY_H
#define PLUGIN_REGISTRY_H

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include "module_runtime.h"

struct PluginInfo {
    std::string path;
    std::vector<std::string> dependencies;
    // Direct reverse edges — names of plugins whose `dependencies` list
    // includes this plugin. Kept in sync with `dependencies` across every
    // graph mutation by PluginRegistry itself; callers never populate it
    // directly. Use PluginRegistry::pluginDependents() for transitive walks.
    std::vector<std::string> dependents;
    bool loaded = false;
    // Owning runtime, set when a module is loaded via loadPluginInternal().
    // Null when loaded directly via markLoaded(name) (test/external scenarios).
    std::shared_ptr<LogosCore::ModuleRuntime> runtime;
    LogosCore::LoadedModuleHandle handle;
};

class PluginRegistry {
public:
    void setPluginsDir(const std::string& dir);
    void addPluginsDir(const std::string& dir);
    std::vector<std::string> pluginsDirs() const;

    void discoverInstalledModules();
    std::string processPlugin(const std::string& pluginPath);

    bool isKnown(const std::string& name) const;
    std::string pluginPath(const std::string& name) const;
    // Forward-edge accessor. `recursive=false` returns the direct
    // dependencies stored on PluginInfo. `recursive=true` walks the forward
    // graph breadth-first and returns every transitive dependency. Unknown
    // names yield an empty list. Traversal is cycle- and diamond-safe.
    std::vector<std::string> pluginDependencies(const std::string& name,
                                                bool recursive = false) const;
    // Reverse-edge accessor. `recursive=false` returns the direct
    // dependents stored on PluginInfo. `recursive=true` walks the reverse
    // graph breadth-first and returns every transitive dependent. Unknown
    // names yield an empty list.
    std::vector<std::string> pluginDependents(const std::string& name,
                                              bool recursive = false) const;
    std::vector<std::string> knownPluginNames() const;
    void registerPlugin(const std::string& name, const std::string& path,
                        const std::vector<std::string>& dependencies = {});
    void registerDependencies(const std::string& name, const std::vector<std::string>& dependencies);

    bool isLoaded(const std::string& name) const;

    // Simple mark-as-loaded (no runtime association). Used by tests and
    // external callers that set up state directly without going through loadPlugin.
    void markLoaded(const std::string& name);

    // Full mark-as-loaded that stores the owning runtime and handle for later
    // use by unloadPlugin(). Should be called from loadPluginInternal().
    void markLoaded(const std::string& name,
                    std::shared_ptr<LogosCore::ModuleRuntime> runtime,
                    LogosCore::LoadedModuleHandle handle);

    void markUnloaded(const std::string& name);
    std::vector<std::string> loadedPluginNames() const;
    void clearLoaded();

    // Returns the runtime that owns the named module, or nullptr if it was
    // loaded without a runtime association (e.g. via markLoaded(name) only).
    std::shared_ptr<LogosCore::ModuleRuntime> runtimeFor(const std::string& name) const;

    void clear();

private:
    std::string processPluginInternal(const std::string& pluginPath);

    // Re-derives every PluginInfo::dependents list by inverting the
    // dependencies edges across m_plugins. Called at the tail of
    // discoverInstalledModules() and processPlugin(), and by any other
    // mutation that can change the graph (including registerPlugin and
    // registerDependencies). Must be called with m_mutex held
    // exclusively. Cost is O(N * avg_deps) — negligible for the module
    // counts we see and simpler than keeping incremental diffs.
    void recomputeDependentsLocked();
    std::vector<std::string> pluginDependenciesLocked(const std::string& name,
                                                      bool recursive) const;
    std::vector<std::string> pluginDependentsLocked(const std::string& name,
                                                    bool recursive) const;

    mutable std::shared_mutex m_mutex;
    std::vector<std::string> m_pluginsDirs;
    std::unordered_map<std::string, PluginInfo> m_plugins;
};

#endif // PLUGIN_REGISTRY_H
