#include "plugin_manager.h"
#include "plugin_registry.h"
#include "dependency_resolver.h"
#include "plugin_launcher.h"
#include <spdlog/spdlog.h>
#include <QString>
#include <mutex>
#include <cassert>
#include <cstring>
#include <unordered_set>
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
        QString capabilityModuleToken = tokenManager.getToken("capability_module");

        static LogosAPI* s_coreApi = nullptr;
        if (!s_coreApi)
            s_coreApi = new LogosAPI("core");

        LogosAPIClient* client = s_coreApi->getClient("capability_module");
        if (!client->informModuleToken(capabilityModuleToken,
                                       QString::fromStdString(name),
                                       QString::fromStdString(token))) {
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

        TokenManager::instance().saveToken(QString::fromStdString(name),
                                           QString::fromStdString(authToken));

        notifyCapabilityModule(name, authToken);

        spdlog::info("Plugin loaded: {}", name);

        return true;
    }

    // Unload helper that assumes loadMutex() is already held by the caller.
    // unloadPluginWithDependents() needs a single lock span so a late-arriving
    // load can't interleave between tearing down the dependents and the target.
    bool unloadPluginInternalLocked(const std::string& name) {
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
        return unloadPluginInternalLocked(std::string(pluginName));
    }

    bool unloadPluginWithDependents(const char* pluginName) {
        std::lock_guard lock(loadMutex());

        std::string name(pluginName);

        if (!registryInstance().isLoaded(name)) {
            spdlog::warn("Cannot unload plugin (not loaded): {}", name);
            return false;
        }

        // Build the set of plugins that need to come down: the target plus
        // every currently-loaded recursive dependent. Materialise the loaded
        // set into a hash once so the membership check below is O(1).
        std::vector<std::string> loadedNames = registryInstance().loadedPluginNames();
        std::unordered_set<std::string> loaded(loadedNames.begin(), loadedNames.end());

        // Reverse dependency walk against the in-process graph. PluginRegistry
        // keeps PluginInfo::dependents in sync with PluginInfo::dependencies
        // across every discovery pass, so we don't need a disk-backed query.
        std::vector<std::string> dependents = registryInstance().pluginDependents(name, /*recursive=*/true);

        std::vector<std::string> teardownSet;
        std::unordered_set<std::string> teardownSetMembers;
        teardownSet.push_back(name);
        teardownSetMembers.insert(name);
        for (const std::string& d : dependents) {
            if (loaded.count(d) && teardownSetMembers.insert(d).second)
                teardownSet.push_back(d);
        }

        // Order leaves-first: resolve load-order for the teardown set, then
        // reverse. Dependents come down before the plugins they depend on.
        std::vector<std::string> loadOrder = DependencyResolver::resolve(
            teardownSet,
            [](const std::string& n) { return registryInstance().isKnown(n); },
            [](const std::string& n) { return registryInstance().pluginDependencies(n); }
        );
        std::vector<std::string> teardownOrder;
        std::unordered_set<std::string> teardownOrderMembers;
        for (auto it = loadOrder.rbegin(); it != loadOrder.rend(); ++it) {
            if (teardownSetMembers.count(*it) && teardownOrderMembers.insert(*it).second)
                teardownOrder.push_back(*it);
        }
        // Safety net: any members not seen by the resolver (shouldn't happen,
        // but don't silently skip them) go to the end.
        for (const std::string& n : teardownSet) {
            if (teardownOrderMembers.insert(n).second)
                teardownOrder.push_back(n);
        }

        bool allSucceeded = true;
        for (const std::string& n : teardownOrder) {
            if (!registryInstance().isLoaded(n)) continue;
            if (!unloadPluginInternalLocked(n)) {
                spdlog::warn("Failed to unload plugin during cascade: {}", n);
                allSucceeded = false;
            }
        }

        return allSucceeded;
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

    std::vector<std::string> getDependencies(const std::string& name, bool recursive) {
        // Filter to known modules to honour the documented contract.
        // PluginInfo::dependencies holds whatever the manifest declares,
        // including names that aren't installed. The reverse-edge accessor
        // doesn't need this treatment because recomputeDependentsLocked only
        // writes known names into PluginInfo::dependents by construction.
        std::vector<std::string> deps = registryInstance().pluginDependencies(name, recursive);
        std::vector<std::string> knownDeps;
        knownDeps.reserve(deps.size());
        for (const std::string& dep : deps) {
            if (registryInstance().isKnown(dep))
                knownDeps.push_back(dep);
        }
        return knownDeps;
    }

    std::vector<std::string> getDependents(const std::string& name, bool recursive) {
        return registryInstance().pluginDependents(name, recursive);
    }

    char** getDependenciesCStr(const char* name, bool recursive) {
        return toNullTerminatedArray(
            getDependencies(std::string(name), recursive));
    }

    char** getDependentsCStr(const char* name, bool recursive) {
        return toNullTerminatedArray(
            getDependents(std::string(name), recursive));
    }
}
