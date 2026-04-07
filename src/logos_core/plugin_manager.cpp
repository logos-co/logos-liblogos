#include "plugin_manager.h"
#include <algorithm>
#include "dependency_resolver.h"
#include "logos_uuid.h"
#include "logos_logging.h"
#include "plugin_launcher.h"
#include "plugin_registry.h"
#include <cassert>
#include <cstring>
#include <mutex>
#include "logos_api.h"
#include "logos_api_client.h"
#include "token_manager.h"
#include <QString>

namespace {
    PluginRegistry& registryInstance()
    {
        static PluginRegistry instance;
        return instance;
    }

    std::mutex& loadMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    char** toNullTerminatedArray(const std::vector<std::string>& list)
    {
        const int count = static_cast<int>(list.size());
        if (count == 0) {
            char** result = new char*[1];
            result[0] = nullptr;
            return result;
        }

        char** result = new char*[count + 1];
        for (int i = 0; i < count; ++i) {
            const std::string& s = list[i];
            result[i] = new char[s.size() + 1];
            std::memcpy(result[i], s.c_str(), s.size() + 1);
        }
        result[count] = nullptr;
        return result;
    }

    void notifyCapabilityModule(const std::string& name, const std::string& token)
    {
        if (!registryInstance().isKnown("capability_module"))
            return;

        TokenManager& tokenManager = TokenManager::instance();
        QString capabilityModuleToken = tokenManager.getToken("capability_module");

        static LogosAPI* s_coreApi = nullptr;
        if (!s_coreApi)
            s_coreApi = new LogosAPI("core");

        LogosAPIClient* client = s_coreApi->getClient("capability_module");
        if (!client->informModuleToken(capabilityModuleToken, QString::fromStdString(name), QString::fromStdString(token))) {
            logos_log_warn("Failed to register token with capability module for: {}", name);
        }
    }

    bool loadPluginInternal(const char* pluginName)
    {
        const std::string name(pluginName);

        if (!registryInstance().isKnown(name)) {
            logos_log_warn("Cannot load unknown plugin: {}", name);
            return false;
        }

        if (registryInstance().isLoaded(name)) {
            logos_log_warn("Plugin already loaded: {}", name);
            return false;
        }

        std::string path = registryInstance().pluginPath(name);

        auto onTerminated = [](const std::string& n) { registryInstance().markUnloaded(n); };

        if (!PluginLauncher::launch(name, path, registryInstance().pluginsDirs(), onTerminated))
            return false;

        const std::string authToken = logos_random_uuid_string();

        if (!PluginLauncher::sendToken(name, authToken))
            return false;

        registryInstance().markLoaded(name);

        TokenManager::instance().saveToken(QString::fromStdString(name), QString::fromStdString(authToken));

        notifyCapabilityModule(name, authToken);

        logos_log_info("Plugin loaded: {}", name);

        return true;
    }
}

namespace PluginManager {

    PluginRegistry& registry()
    {
        return registryInstance();
    }

    void setPluginsDir(const char* plugins_dir)
    {
        assert(plugins_dir != nullptr);
        registryInstance().setPluginsDir(plugins_dir);
    }

    void addPluginsDir(const char* plugins_dir)
    {
        assert(plugins_dir != nullptr);
        registryInstance().addPluginsDir(plugins_dir);
    }

    void discoverInstalledModules()
    {
        registryInstance().discoverInstalledModules();
    }

    std::string processPlugin(const std::string& pluginPath)
    {
        return registryInstance().processPlugin(pluginPath);
    }

    char* processPluginCStr(const char* pluginPath)
    {
        assert(pluginPath != nullptr);
        std::string path(pluginPath);

        std::string pluginName = registryInstance().processPlugin(path);
        if (pluginName.empty()) {
            logos_log_warn("Failed to process plugin: {}", path);
            return nullptr;
        }

        char* result = new char[pluginName.size() + 1];
        std::memcpy(result, pluginName.c_str(), pluginName.size() + 1);
        return result;
    }

    bool loadPlugin(const char* pluginName)
    {
        std::lock_guard lock(loadMutex());
        return loadPluginInternal(pluginName);
    }

    bool loadPluginWithDependencies(const char* pluginName)
    {
        std::lock_guard lock(loadMutex());

        const std::string name(pluginName);

        std::vector<std::string> requested;
        requested.push_back(name);

        std::vector<std::string> resolved = DependencyResolver::resolve(
            requested,
            [](const std::string& n) { return registryInstance().isKnown(n); },
            [](const std::string& n) { return registryInstance().pluginDependencies(n); });

        if (resolved.empty()
            || std::find(resolved.begin(), resolved.end(), name) == resolved.end()) {
            logos_log_warn("Cannot resolve dependencies for: {}", name);
            return false;
        }

        bool allSucceeded = true;
        for (const std::string& moduleName : resolved) {
            if (registryInstance().isLoaded(moduleName))
                continue;
            if (!loadPluginInternal(moduleName.c_str())) {
                logos_log_warn("Failed to load plugin: {}", moduleName);
                allSucceeded = false;
            }
        }

        return allSucceeded;
    }

    bool initializeCapabilityModule()
    {
        std::lock_guard lock(loadMutex());

        if (!registryInstance().isKnown("capability_module"))
            return false;

        if (!loadPluginInternal("capability_module")) {
            logos_log_warn("Failed to load capability module");
            return false;
        }

        return true;
    }

    bool unloadPlugin(const char* pluginName)
    {
        std::lock_guard lock(loadMutex());

        const std::string name(pluginName);

        if (!registryInstance().isLoaded(name)) {
            logos_log_warn("Cannot unload plugin (not loaded): {}", name);
            return false;
        }

        if (!PluginLauncher::hasProcess(name)) {
            logos_log_warn("No process found for plugin: {}", name);
            return false;
        }

        PluginLauncher::terminate(name);
        registryInstance().markUnloaded(name);

        logos_log_info("Plugin unloaded: {}", name);
        return true;
    }

    void terminateAll()
    {
        std::lock_guard lock(loadMutex());
        PluginLauncher::terminateAll();
        registryInstance().clearLoaded();
    }

    void clear()
    {
        std::lock_guard lock(loadMutex());
        PluginLauncher::terminateAll();
        registryInstance().clear();
    }

    char** getLoadedPluginsCStr()
    {
        return toNullTerminatedArray(registryInstance().loadedPluginNames());
    }

    char** getKnownPluginsCStr()
    {
        std::vector<std::string> known = registryInstance().knownPluginNames();
        if (known.empty())
            logos_log_warn("No known plugins to return");
        return toNullTerminatedArray(known);
    }

    bool isPluginLoaded(const std::string& name)
    {
        return registryInstance().isLoaded(name);
    }

    std::unordered_map<std::string, int64_t> getPluginProcessIds()
    {
        return PluginLauncher::getAllProcessIds();
    }

    std::vector<std::string> resolveDependencies(const std::vector<std::string>& requestedModules)
    {
        return DependencyResolver::resolve(
            requestedModules,
            [](const std::string& n) { return registryInstance().isKnown(n); },
            [](const std::string& n) { return registryInstance().pluginDependencies(n); });
    }

}
