#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

class PluginRegistry;

namespace PluginManager {
    PluginRegistry& registry();

    void setPluginsDir(const char* plugins_dir);
    void addPluginsDir(const char* plugins_dir);
    void setPersistenceBasePath(const char* path);

    void discoverInstalledModules();

    std::string processPlugin(const std::string& pluginPath);
    char* processPluginCStr(const char* pluginPath);
    bool loadPlugin(const char* pluginName);
    bool loadPluginWithDependencies(const char* pluginName);
    bool initializeCapabilityModule();
    bool unloadPlugin(const char* pluginName);
    void terminateAll();
    void clear();

    char** getLoadedPluginsCStr();
    char** getKnownPluginsCStr();

    bool isPluginLoaded(const std::string& name);
    std::unordered_map<std::string, int64_t> getPluginProcessIds();

    std::vector<std::string> resolveDependencies(const std::vector<std::string>& requestedModules);
}

#endif // PLUGIN_MANAGER_H
