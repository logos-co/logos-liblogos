#ifndef PLUGIN_LAUNCHER_H
#define PLUGIN_LAUNCHER_H

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>

namespace PluginLauncher {
    using OnTerminatedFn = std::function<void(const std::string& name)>;

    bool launch(const std::string& name, const std::string& pluginPath,
                const std::vector<std::string>& pluginsDirs,
                const std::string& instancePersistencePath,
                OnTerminatedFn onTerminated);
    bool sendToken(const std::string& name, const std::string& token);
    void terminate(const std::string& name);
    void terminateAll();
    bool hasProcess(const std::string& name);
    std::unordered_map<std::string, int64_t> getAllProcessIds();
}

#endif // PLUGIN_LAUNCHER_H
