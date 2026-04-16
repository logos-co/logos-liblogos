#ifndef PLUGIN_REGISTRY_H
#define PLUGIN_REGISTRY_H

#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>

struct PluginInfo {
    std::string path;
    std::vector<std::string> dependencies;
    // Direct reverse edges — names of plugins whose `dependencies` list
    // includes this plugin. Kept in sync with `dependencies` across every
    // graph mutation by PluginRegistry itself; callers never populate it
    // directly. Use PluginRegistry::pluginDependents() for transitive walks.
    std::vector<std::string> dependents;
    bool loaded = false;
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
    void markLoaded(const std::string& name);
    void markUnloaded(const std::string& name);
    std::vector<std::string> loadedPluginNames() const;
    void clearLoaded();

    void clear();

private:
    std::string processPluginInternal(const std::string& pluginPath);

    // Re-derives every PluginInfo::dependents list by inverting the
    // dependencies edges across m_plugins. Called at the tail of
    // discoverInstalledModules() and processPlugin(), and by any other
    // mutation that can change the graph (registerPlugin when deps were
    // passed, registerDependencies). Must be called with m_mutex held
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
