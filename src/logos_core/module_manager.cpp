#include "module_manager.h"
#include "module_registry.h"
#include "access_policy.h"
#include "dependency_resolver.h"
#include "runtime_registry.h"
#include "composite_runtime.h"
#include "containers/subprocess/subprocess_container.h"
#include "runtimes/runtime_qt/qt_plugin_runtime.h"
#include <spdlog/spdlog.h>
#include <mutex>
#include <cassert>
#include <cstring>
#include <unordered_set>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_transport_config_json.h"
#include "token_manager.h"
#include "instance_persistence.h"
#include <QString>
#include <QStringList>
#include <QVariant>

namespace {
    ModuleRegistry& registryInstance() {
        static ModuleRegistry instance;
        return instance;
    }

    std::mutex& loadMutex() {
        static std::mutex mutex;
        return mutex;
    }

    // Per-module transport set, keyed by module name. Set by the
    // daemon before the corresponding module loads (capability_module
    // before logos_core_start; user modules before loadModule). Empty
    // = inherit the global default. See module_manager.h for details.
    std::unordered_map<std::string, std::string>& moduleTransportsMap() {
        static std::unordered_map<std::string, std::string> m;
        return m;
    }

    std::string& persistenceBasePath() {
        static std::string path;
        return path;
    }

    // Raw access-policy JSON, set before logos_core_start(); pushed to
    // capability_module once it loads. Guarded by loadMutex().
    std::string& accessPolicyJson() {
        static std::string s;
        return s;
    }

    LogosCore::RuntimeRegistry& runtimeRegistry() {
        static LogosCore::RuntimeRegistry reg;
        static std::once_flag initFlag;
        std::call_once(initFlag, []() {
            auto container = std::make_shared<SubprocessContainer>();
            auto loader    = std::make_shared<QtPluginRuntime>();
            reg.registerRuntime(std::make_shared<LogosCore::CompositeRuntime>(container, loader));
        });
        return reg;
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

    // Dial capability_module from a long-lived "core" LogosAPI. Prefer the
    // operator's first configured transport; fall back to the global
    // default (LocalSocket). Needed because the single-arg getClient()
    // always uses the global default, which hangs against a tcp-only
    // capability_module that never bound a LocalSocket.
    LogosAPIClient* capabilityModuleClient() {
        static LogosAPI* s_coreApi = nullptr;
        if (!s_coreApi)
            s_coreApi = new LogosAPI(std::string("core"));

        if (auto it = moduleTransportsMap().find("capability_module");
            it != moduleTransportsMap().end() && !it->second.empty()) {
            const auto ts = logos::transportSetFromJsonString(it->second);
            if (!ts.empty()) {
                return s_coreApi->getClient(
                    QStringLiteral("capability_module"), ts.front());
            }
        }
        return s_coreApi->getClient(std::string("capability_module"));
    }

    // Parse the stored access policy and register its per-target restrictions
    // with capability_module. Only "enforce" mode registers anything; an
    // empty/unparseable policy leaves it unrestricted. Best-effort (failures
    // logged, not fatal).
    void pushAccessRestrictionsToCapabilityModule() {
        if (accessPolicyJson().empty())
            return;
        if (!registryInstance().isLoaded("capability_module"))
            return;

        auto policy = LogosCore::parseAccessPolicy(accessPolicyJson());
        if (!policy) {
            spdlog::warn("Access policy is not valid JSON — not enforcing any restrictions");
            return;
        }
        if (!policy->enforce())
            return;

        const std::string capabilityModuleToken =
            TokenManager::instance().getToken(std::string("capability_module"));
        LogosAPIClient* client = capabilityModuleClient();

        for (const auto& restriction : policy->restrictions) {
            QStringList allowedCallers;
            for (const auto& caller : restriction.allowedCallers)
                allowedCallers.append(QString::fromStdString(caller));

            // The token is the trusted-channel proof registerRestriction
            // verifies — only core holds it.
            const QVariant result = client->invokeRemoteMethod(
                QStringLiteral("capability_module"),
                QStringLiteral("registerRestriction"),
                QVariant(QString::fromStdString(capabilityModuleToken)),
                QVariant(QString::fromStdString(restriction.target)),
                QVariant(allowedCallers));

            if (!result.toBool()) {
                spdlog::warn("Failed to register access restriction for target: {}",
                             restriction.target);
            } else {
                spdlog::info("Registered access restriction for target: {} ({} allowed callers)",
                             restriction.target, restriction.allowedCallers.size());
            }
        }
    }

    void notifyCapabilityModule(const std::string& name, const std::string& token) {
        if (!registryInstance().isLoaded("capability_module"))
            return;

        TokenManager& tokenManager = TokenManager::instance();
        std::string capabilityModuleToken = tokenManager.getToken(std::string("capability_module"));

        LogosAPIClient* client = capabilityModuleClient();

        if (!client->informModuleToken(capabilityModuleToken, name, token)) {
            spdlog::warn("Failed to register token with capability module for: {}", name);
        }
    }

    bool loadModuleInternal(const char* moduleName) {
        std::string name(moduleName);

        if (!registryInstance().isKnown(name)) {
            spdlog::warn("Cannot load unknown module: {}", name);
            return false;
        }

        // "Already loaded" is a successful no-op, not a failure.
        // Callers (basecamp's PluginLoader::loadCoreDependencies,
        // logoscore-cli, etc.) use loadModule as "ensure loaded";
        // returning false here aborted UI-plugin loads whose core
        // dependency had been pre-loaded at startup (e.g. clicking
        // the package-manager launcher after basecamp pre-loaded
        // `package_manager`).
        if (registryInstance().isLoaded(name)) {
            spdlog::debug("Module already loaded (no-op): {}", name);
            return true;
        }

        std::string modPath = registryInstance().modulePath(name);

        // Build a descriptor for the runtime to inspect.
        LogosCore::ModuleDescriptor desc;
        desc.name        = name;
        desc.path        = modPath;
        desc.format      = "qt-plugin";
        desc.dependencies = registryInstance().moduleDependencies(name);
        desc.modulesDirs  = registryInstance().modulesDirs();

        if (!persistenceBasePath().empty()) {
            auto info = ModuleLib::InstancePersistence::resolveInstance(
                persistenceBasePath(), name);
            desc.instancePersistencePath = info.persistencePath;
        }

        // Per-module transport set, if the daemon registered one before
        // calling load. The runtime threads it through to the child via
        // a CLI argument so the child's LogosAPIProvider binds the right
        // listeners. Modules without an entry inherit the global default.
        if (auto it = moduleTransportsMap().find(name);
            it != moduleTransportsMap().end()) {
            desc.transportSetJson = it->second;
        }

        auto rt = runtimeRegistry().select(desc);
        if (!rt) {
            spdlog::warn("No runtime available to load module: {}", name);
            return false;
        }

        auto onTerminated = [](const std::string& n) {
            registryInstance().markUnloaded(n);
        };

        LogosCore::LoadedModuleHandle handle;
        if (!rt->load(desc, onTerminated, handle))
            return false;

        std::string authToken = boost::uuids::to_string(boost::uuids::random_generator()());

        if (!rt->sendToken(name, authToken)) {
            rt->terminate(name);
            return false;
        }

        registryInstance().markLoaded(name, rt, std::move(handle));

        TokenManager::instance().saveToken(name, authToken);

        notifyCapabilityModule(name, authToken);

        spdlog::info("Module loaded: {}", name);

        return true;
    }

    // Unload helper that assumes loadMutex() is already held by the caller.
    // unloadModuleWithDependents() needs a single lock span so a late-arriving
    // load can't interleave between tearing down the dependents and the target.
    bool unloadModuleInternalLocked(const std::string& name) {
        if (!registryInstance().isLoaded(name)) {
            spdlog::warn("Cannot unload module (not loaded): {}", name);
            return false;
        }

        auto rt = registryInstance().runtimeFor(name);
        if (rt) {
            if (!rt->hasModule(name)) {
                spdlog::warn("No module entry found for module: {}", name);
                return false;
            }
            rt->terminate(name);
        } else {
            // Fallback: module was loaded via markLoaded(name) directly
            // (test scenarios or external setup). Use SubprocessContainer directly.
            if (!SubprocessContainer::hasProcess(name)) {
                spdlog::warn("No process found for module: {}", name);
                return false;
            }
            SubprocessContainer::terminateProcess(name);
        }

        registryInstance().markUnloaded(name);

        spdlog::info("Module unloaded: {}", name);
        return true;
    }
}

namespace ModuleManager {

    ModuleRegistry& registry() {
        return registryInstance();
    }

    LogosCore::RuntimeRegistry& runtimes() {
        return runtimeRegistry();
    }

    void setModulesDir(const char* modules_dir) {
        assert(modules_dir != nullptr);
        registryInstance().setModulesDir(std::string(modules_dir));
    }

    void addModulesDir(const char* modules_dir) {
        assert(modules_dir != nullptr);
        registryInstance().addModulesDir(std::string(modules_dir));
    }

    void setPersistenceBasePath(const char* path) {
        assert(path != nullptr);
        persistenceBasePath() = std::string(path);
    }

    void setModuleTransports(const std::string& moduleName,
                             const std::string& transportSetJson) {
        // Same mutex as loadModule()'s read of the map (see line ~122
        // for the lookup). Without this, an operator can race with
        // an in-flight loadModule and the child gets garbled JSON
        // (or sees an empty transport set after the operator
        // overwrote what the child was about to read).
        std::lock_guard<std::mutex> g(loadMutex());
        if (transportSetJson.empty())
            moduleTransportsMap().erase(moduleName);
        else
            moduleTransportsMap()[moduleName] = transportSetJson;
    }

    void setAccessPolicy(const std::string& policyJson) {
        std::lock_guard<std::mutex> g(loadMutex());  // guards the read at push time
        accessPolicyJson() = policyJson;
        // Validate eagerly so a malformed policy is flagged at set time, not
        // silently at capability_module load. Stored verbatim regardless.
        if (!policyJson.empty() && !LogosCore::parseAccessPolicy(policyJson)) {
            spdlog::warn("logos_core_set_access_policy: policy is not valid JSON "
                         "— no restrictions will be enforced");
        }
    }

    void discoverInstalledModules() {
        registryInstance().discoverInstalledModules();
    }

    std::string processModule(const std::string& modulePath) {
        return registryInstance().processModule(modulePath);
    }

    char* processModuleCStr(const char* modulePath) {
        std::string path(modulePath);

        std::string moduleName = registryInstance().processModule(path);
        if (moduleName.empty()) {
            spdlog::warn("Failed to process module: {}", path);
            return nullptr;
        }

        char* result = new char[moduleName.size() + 1];
        strcpy(result, moduleName.c_str());
        return result;
    }

    bool loadModule(const char* moduleName) {
        std::lock_guard lock(loadMutex());
        return loadModuleInternal(moduleName);
    }

    bool loadModuleWithDependencies(const char* moduleName) {
        std::lock_guard lock(loadMutex());

        std::string name(moduleName);

        std::vector<std::string> requested;
        requested.push_back(name);

        auto resolved = DependencyResolver::resolve(
            requested,
            [](const std::string& n) { return registryInstance().isKnown(n); },
            [](const std::string& n) { return registryInstance().moduleDependencies(n); }
        );

        // Treat missing dependencies and cycles as hard failures.
        // The header contract (logos_core.h) promises "returns 0 when
        // dependency resolution fails", so we must not proceed with a
        // partial order that silently dropped unknown deps or cycled.
        if (!resolved.ok()) {
            spdlog::warn("Cannot resolve dependencies for: {}", name);
            return false;
        }

        bool nameFound = false;
        for (const auto& r : resolved.order) {
            if (r == name) { nameFound = true; break; }
        }

        if (resolved.order.empty() || !nameFound) {
            spdlog::warn("Cannot resolve dependencies for: {}", name);
            return false;
        }

        bool allSucceeded = true;
        for (const std::string& moduleName : resolved.order) {
            if (!loadModuleInternal(moduleName.c_str())) {
                spdlog::warn("Failed to load module: {}", moduleName);
                allSucceeded = false;
            }
        }

        return allSucceeded;
    }

    bool initializeCapabilityModule() {
        std::lock_guard lock(loadMutex());

        if (!registryInstance().isKnown("capability_module"))
            return false;

        if (!loadModuleInternal("capability_module")) {
            spdlog::warn("Failed to load capability module");
            return false;
        }

        // Register restrictions now, before any other module can call out.
        pushAccessRestrictionsToCapabilityModule();

        return true;
    }

    bool unloadModule(const char* moduleName) {
        std::lock_guard lock(loadMutex());
        return unloadModuleInternalLocked(std::string(moduleName));
    }

    bool unloadModuleWithDependents(const char* moduleName) {
        std::lock_guard lock(loadMutex());

        std::string name(moduleName);

        if (!registryInstance().isLoaded(name)) {
            spdlog::warn("Cannot unload module (not loaded): {}", name);
            return false;
        }

        // Build the set of modules that need to come down: the target plus
        // every currently-loaded recursive dependent. Materialise the loaded
        // set into a hash once so the membership check below is O(1).
        std::vector<std::string> loadedNames = registryInstance().loadedModuleNames();
        std::unordered_set<std::string> loaded(loadedNames.begin(), loadedNames.end());

        // Reverse dependency walk against the in-process graph. ModuleRegistry
        // keeps ModuleInfo::dependents in sync with ModuleInfo::dependencies
        // across every discovery pass, so we don't need a disk-backed query.
        std::vector<std::string> dependents = registryInstance().moduleDependents(name, /*recursive=*/true);

        std::vector<std::string> teardownSet;
        std::unordered_set<std::string> teardownSetMembers;
        teardownSet.push_back(name);
        teardownSetMembers.insert(name);
        for (const std::string& d : dependents) {
            if (loaded.count(d) && teardownSetMembers.insert(d).second)
                teardownSet.push_back(d);
        }

        // Order leaves-first: resolve load-order for the teardown set, then
        // reverse. Dependents come down before the modules they depend on.
        // Teardown is best-effort — we use .order and ignore resolution errors
        // (missing deps / cycles) because we need to tear down what we can.
        std::vector<std::string> loadOrder = DependencyResolver::resolve(
            teardownSet,
            [](const std::string& n) { return registryInstance().isKnown(n); },
            [](const std::string& n) { return registryInstance().moduleDependencies(n); }
        ).order;
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
            if (!unloadModuleInternalLocked(n)) {
                spdlog::warn("Failed to unload module during cascade: {}", n);
                allSucceeded = false;
            }
        }

        return allSucceeded;
    }

    void terminateAll() {
        std::lock_guard lock(loadMutex());
        runtimeRegistry().terminateAll();
        registryInstance().clearLoaded();
    }

    void clear() {
        std::lock_guard lock(loadMutex());
        runtimeRegistry().terminateAll();
        registryInstance().clear();
        // Per-module transport overrides are part of the manager's
        // mutable state — without clearing them here, a daemon
        // restart in the same process (or a unit test that calls
        // clear() between scenarios) would inherit the previous
        // run's transport map and bind unexpected ports.
        moduleTransportsMap().clear();
        accessPolicyJson().clear();  // same rationale — don't leak across restarts
    }

    char** getLoadedModulesCStr() {
        return toNullTerminatedArray(registryInstance().loadedModuleNames());
    }

    char** getKnownModulesCStr() {
        std::vector<std::string> known = registryInstance().knownModuleNames();
        if (known.empty()) {
            spdlog::warn("No known modules to return");
        }
        return toNullTerminatedArray(known);
    }

    bool isModuleLoaded(const std::string& name) {
        return registryInstance().isLoaded(name);
    }

    std::unordered_map<std::string, int64_t> getModuleProcessIds() {
        return runtimeRegistry().getAllPids();
    }

    std::vector<std::string> resolveDependencies(const std::vector<std::string>& requestedModules) {
        return DependencyResolver::resolve(
            requestedModules,
            [](const std::string& name) { return registryInstance().isKnown(name); },
            [](const std::string& name) { return registryInstance().moduleDependencies(name); }
        ).order;
    }

    std::vector<std::string> getDependencies(const std::string& name, bool recursive) {
        std::vector<std::string> deps = registryInstance().moduleDependencies(name, recursive);
        std::vector<std::string> knownDeps;
        knownDeps.reserve(deps.size());
        for (const std::string& dep : deps) {
            if (registryInstance().isKnown(dep))
                knownDeps.push_back(dep);
        }
        return knownDeps;
    }

    std::vector<std::string> getDependents(const std::string& name, bool recursive) {
        return registryInstance().moduleDependents(name, recursive);
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
