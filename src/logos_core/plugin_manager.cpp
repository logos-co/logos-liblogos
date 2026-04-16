#include "plugin_manager.h"
#include "plugin_registry.h"
#include "dependency_resolver.h"
#include "plugin_launcher.h"
#include <spdlog/spdlog.h>
#include <QString>
#include <mutex>
#include <cassert>
#include <cstring>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "logos_api.h"
#include "logos_api_client.h"
#include "token_manager.h"
#include "instance_persistence.h"

namespace {
    PluginRegistry& registryInstance() {
        static PluginRegistry instance;
        return instance;
    }

    std::mutex& loadMutex() {
        static std::mutex mutex;
        return mutex;
    }

    std::string& persistenceBasePath() {
        static std::string path;
        return path;
    }

    char** toNullTerminatedArray(const std::vector<std::string>& list) {
        int count = static_cast<int>(list.size());
        if (count == 0) {
            char** result = new char*[1];
            result[0] = nullptr;
            return result;
        }

        char** result = new char*[count + 1];
        for (int i = 0; i < count; ++i) {
            result[i] = new char[list[i].size() + 1];
            strcpy(result[i], list[i].c_str());
        }
        result[count] = nullptr;
        return result;
    }

    void notifyCapabilityModule(const std::string& name, const std::string& token) {
        if (!registryInstance().isLoaded("capability_module"))
            return;

        TokenManager& tokenManager = TokenManager::instance();
        std::string capabilityModuleToken = tokenManager.getToken(std::string("capability_module"));

        static LogosAPI* s_coreApi = nullptr;
        if (!s_coreApi)
            s_coreApi = new LogosAPI(std::string("core"));

        LogosAPIClient* client = s_coreApi->getClient(std::string("capability_module"));
        if (!client->informModuleToken(capabilityModuleToken, name, token)) {
            spdlog::warn("Failed to register token with capability module for: {}", name);
        }
    }

    bool loadPluginInternal(const char* pluginName) {
        std::string name(pluginName);

        if (!registryInstance().isKnown(name)) {
            spdlog::warn("Cannot load unknown plugin: {}", name);
            return false;
        }

        if (registryInstance().isLoaded(name)) {
            spdlog::warn("Plugin already loaded: {}", name);
            return false;
        }

        std::string pluginPath = registryInstance().pluginPath(name);

        // Resolve instance persistence path if a base path has been configured
        std::string instancePersistencePath;
        if (!persistenceBasePath().empty()) {
            auto info = ModuleLib::InstancePersistence::resolveInstance(
                QString::fromStdString(persistenceBasePath()),
                QString::fromStdString(name));
            instancePersistencePath = info.persistencePath.toStdString();
        }

        auto onTerminated = [](const std::string& n) {
            registryInstance().markUnloaded(n);
        };

        if (!PluginLauncher::launch(name, pluginPath, registryInstance().pluginsDirs(),
                                    instancePersistencePath, onTerminated))
            return false;

        std::string authToken = boost::uuids::to_string(boost::uuids::random_generator()());

        if (!PluginLauncher::sendToken(name, authToken))
            return false;

        registryInstance().markLoaded(name);

        TokenManager::instance().saveToken(name, authToken);

        notifyCapabilityModule(name, authToken);

        spdlog::info("Plugin loaded: {}", name);

        return true;
    }
}

namespace PluginManager {

    PluginRegistry& registry() {
        return registryInstance();
    }

    void setPluginsDir(const char* plugins_dir) {
        assert(plugins_dir != nullptr);
        registryInstance().setPluginsDir(std::string(plugins_dir));
    }

    void addPluginsDir(const char* plugins_dir) {
        assert(plugins_dir != nullptr);
        registryInstance().addPluginsDir(std::string(plugins_dir));
    }

    void setPersistenceBasePath(const char* path) {
        assert(path != nullptr);
        persistenceBasePath() = std::string(path);
    }

    void discoverInstalledModules() {
        registryInstance().discoverInstalledModules();
    }

    std::string processPlugin(const std::string& pluginPath) {
        return registryInstance().processPlugin(pluginPath);
    }

    char* processPluginCStr(const char* pluginPath) {
        std::string path(pluginPath);

        std::string pluginName = registryInstance().processPlugin(path);
        if (pluginName.empty()) {
            spdlog::warn("Failed to process plugin: {}", path);
            return nullptr;
        }

        char* result = new char[pluginName.size() + 1];
        strcpy(result, pluginName.c_str());
        return result;
    }

    bool loadPlugin(const char* pluginName) {
        std::lock_guard lock(loadMutex());
        return loadPluginInternal(pluginName);
    }

    bool loadPluginWithDependencies(const char* pluginName) {
        std::lock_guard lock(loadMutex());

        std::string name(pluginName);

        std::vector<std::string> requested;
        requested.push_back(name);

        std::vector<std::string> resolved = DependencyResolver::resolve(
            requested,
            [](const std::string& n) { return registryInstance().isKnown(n); },
            [](const std::string& n) { return registryInstance().pluginDependencies(n); }
        );

        bool nameFound = false;
        for (const auto& r : resolved) {
            if (r == name) { nameFound = true; break; }
        }

        if (resolved.empty() || !nameFound) {
            spdlog::warn("Cannot resolve dependencies for: {}", name);
            return false;
        }

        bool allSucceeded = true;
        for (const std::string& moduleName : resolved) {
            if (registryInstance().isLoaded(moduleName))
                continue;
            if (!loadPluginInternal(moduleName.c_str())) {
                spdlog::warn("Failed to load plugin: {}", moduleName);
                allSucceeded = false;
            }
        }

        return allSucceeded;
    }

    bool initializeCapabilityModule() {
        std::lock_guard lock(loadMutex());

        if (!registryInstance().isKnown("capability_module"))
            return false;

        if (!loadPluginInternal("capability_module")) {
            spdlog::warn("Failed to load capability module");
            return false;
        }

        return true;
    }

    bool unloadPlugin(const char* pluginName) {
        std::lock_guard lock(loadMutex());

        std::string name(pluginName);

        if (!registryInstance().isLoaded(name)) {
            spdlog::warn("Cannot unload plugin (not loaded): {}", name);
            return false;
        }

        if (!PluginLauncher::hasProcess(name)) {
            spdlog::warn("No process found for plugin: {}", name);
            return false;
        }

        PluginLauncher::terminate(name);
        registryInstance().markUnloaded(name);

        spdlog::info("Plugin unloaded: {}", name);
        return true;
    }

    void terminateAll() {
        std::lock_guard lock(loadMutex());
        PluginLauncher::terminateAll();
        registryInstance().clearLoaded();
    }

    void clear() {
        std::lock_guard lock(loadMutex());
        PluginLauncher::terminateAll();
        registryInstance().clear();
    }

    char** getLoadedPluginsCStr() {
        return toNullTerminatedArray(registryInstance().loadedPluginNames());
    }

    char** getKnownPluginsCStr() {
        std::vector<std::string> known = registryInstance().knownPluginNames();
        if (known.empty()) {
            spdlog::warn("No known plugins to return");
        }
        return toNullTerminatedArray(known);
    }

    bool isPluginLoaded(const std::string& name) {
        return registryInstance().isLoaded(name);
    }

    std::unordered_map<std::string, int64_t> getPluginProcessIds() {
        return PluginLauncher::getAllProcessIds();
    }

    std::vector<std::string> resolveDependencies(const std::vector<std::string>& requestedModules) {
        return DependencyResolver::resolve(
            requestedModules,
            [](const std::string& name) { return registryInstance().isKnown(name); },
            [](const std::string& name) { return registryInstance().pluginDependencies(name); }
        );
    }

}
