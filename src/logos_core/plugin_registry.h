#ifndef PLUGIN_REGISTRY_H
#define PLUGIN_REGISTRY_H

#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>

struct PluginInfo {
    std::string path;
    std::vector<std::string> dependencies;
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
    std::vector<std::string> pluginDependencies(const std::string& name) const;
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

    mutable std::shared_mutex m_mutex;
    std::vector<std::string> m_pluginsDirs;
    std::unordered_map<std::string, PluginInfo> m_plugins;
};

#endif // PLUGIN_REGISTRY_H
