#include "module_manager.h"
#include "module_registry.h"
#include "access_policy.h"
#include "dependency_resolver.h"
#include "module_loader_registry.h"
#include "composite_module_loader.h"
#include <logos_container/container_factory.h>
#include <logos_module_loader/format_loader_factory.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <cassert>
#include <cstring>
#include <optional>
#include <unordered_set>
#include <QCoreApplication>
#include <QObject>
#include <QPointer>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_module.h"
#include "logos_protocol.h"
#include "protocol_gate.h"
#include "logos_transport_config_json.h"
#include "token_manager.h"
#include "instance_persistence.h"

namespace ModuleManager {
    // Defined later in this TU (in namespace ModuleManager). Forward-declared
    // here so the loader's onTerminated hook — installed from the anonymous
    // namespace's loadModuleInternal — can reach the capability crash supervisor.
    void maybeScheduleCapabilityRestart(const std::string& name);
}

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

    // Both guarded by loadMutex(). parsedEnforcePolicy is set only in enforce mode.
    std::string& accessPolicyJson() {
        static std::string s;
        return s;
    }

    std::optional<LogosCore::AccessPolicy>& parsedEnforcePolicy() {
        static std::optional<LogosCore::AccessPolicy> p;
        return p;
    }

    // Always allowed past the dependency check, so they're never locked out.
    const std::vector<std::string> kTrustedCallers = {"core", "core_service"};

    // Never restricted as targets, even if an explicit policy names them.
    // TODO: re-eval this; probably is required to restrict core/core_service
    const std::vector<std::string> kExemptTargets =
        {"capability_module", "core", "core_service"};

    // ── capability_module crash supervisor ──────────────────────────────────
    //
    // capability_module is the token authority: every inter-module call needs a
    // token minted/validated by it. If its subprocess dies (it crashed with a
    // QtRO SIGSEGV in one incident) and nothing brings it back, ALL token
    // exchange is dead system-wide until the app restarts. The supervisor below
    // detects an *unexpected* exit of capability_module and re-provisions it
    // (respawn + re-teach every loaded module's token). It is deliberately
    // scoped to capability_module only — ordinary user modules (e.g. a module
    // with a deterministic panic) must NOT be auto-restarted, or we would just
    // replay their crash and hide the real bug.
    bool isAutoRestartTarget(const std::string& name) {
        return name == "capability_module";
    }

    // Set during logos_core_cleanup / clear() / terminateAll() so a subprocess
    // exit that races our own teardown never schedules a respawn into a
    // tearing-down process tree.
    std::atomic<bool>& shuttingDown() {
        static std::atomic<bool> s{false};
        return s;
    }

    // NOTE on crash-vs-deliberate-unload: the loader's onTerminated fires ONLY
    // for spontaneous exits. A deliberate unload/terminate sets the container
    // entry's `cancelled` flag, and the container gates the onFinished/onError
    // callback on `!cancelled` (subprocess_container.cpp) — so a user unload of
    // capability_module never reaches the crash hook below. Combined with the
    // shuttingDown() latch (set during clear()/terminateAll()/cleanup), that
    // covers every deliberate teardown without a separate bookkeeping set.

    // Main-thread QObject used to hop the respawn off the loader's background
    // thread and onto the frontend's Qt event loop, where all QtRO / loadMutex
    // work is safe. Created on the logos_core_start thread in
    // initializeCapabilityModule(). Null in a headless test with no event loop.
    QPointer<QObject>& restartDispatcher() {
        static QPointer<QObject> d;
        return d;
    }

    // Built-in default loader, composed from the container + format-loader the
    // build linked in. The concrete implementations are chosen at link time via
    // the contract factory seams (LogosCore::makeContainer / makeFormatLoader);
    // the core names no specific container or loader. Frontends can still
    // register additional loaders via ModuleManager::loaders().registerLoader().
    LogosCore::ModuleLoaderRegistry& loaderRegistry() {
        static LogosCore::ModuleLoaderRegistry reg;
        static std::once_flag initFlag;
        std::call_once(initFlag, []() {
            auto container = LogosCore::makeContainer();
            auto loader    = LogosCore::makeFormatLoader();
            if (container && loader)
                reg.registerLoader(std::make_shared<LogosCore::CompositeModuleLoader>(container, loader));
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
    // The long-lived "core" LogosAPI used to dial capability_module. Held in a
    // resettable slot so a capability respawn can drop the cached client — its
    // QRemoteObjectNode is latched onto the DEAD subprocess's socket and would
    // otherwise 20s-time-out against the fresh one. Only ever touched under
    // loadMutex() (capabilityModuleClient callers) or on the main thread during
    // reprovision, so no extra synchronisation is needed.
    LogosAPI*& coreApiSlot() {
        static LogosAPI* s_coreApi = nullptr;
        return s_coreApi;
    }

    // Drop the cached core->capability client so the next capabilityModuleClient()
    // rebuilds it (fresh consumer + node) against the respawned subprocess.
    void resetCapabilityModuleClient() {
        LogosAPI* old = coreApiSlot();
        coreApiSlot() = nullptr;
        if (old)
            old->deleteLater();  // defer: may be reached from a call stack that still unwinds through it
    }

    LogosAPIClient* capabilityModuleClient() {
        LogosAPI*& s_coreApi = coreApiSlot();
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

    // Token authenticates the call. Best-effort; assumes capability_module loaded.
    void registerRestrictionRpc(const std::string& target,
                                const std::vector<std::string>& callers) {
        nlohmann::json args = nlohmann::json::array();
        args.push_back(TokenManager::instance().getToken(std::string("capability_module")));
        args.push_back(target);
        args.push_back(callers);

        nlohmann::json result = capabilityModuleClient()->invokeRemoteMethod(
            std::string("capability_module"),
            std::string("registerRestriction"),
            args);

        if (!result.is_boolean() || !result.get<bool>())
            spdlog::warn("Failed to register access restriction for target: {}", target);
        else
            spdlog::info("Registered access restriction for target: {} ({} allowed callers)",
                         target, callers.size());
    }

    // Explicit-policy restrictions, including targets not yet loaded (the
    // derived path covers only loaded ones).
    void pushAccessRestrictionsToCapabilityModule() {
        if (!registryInstance().isLoaded("capability_module"))
            return;
        const auto& policy = parsedEnforcePolicy();
        if (!policy)
            return;

        for (const auto& restriction : policy->restrictions) {
            if (std::find(kExemptTargets.begin(), kExemptTargets.end(),
                          restriction.target) != kExemptTargets.end())
                continue;
            registerRestrictionRpc(restriction.target, restriction.allowedCallers);
        }
    }

    // A module may only call modules it declared as a dependency, so `target`'s
    // allowed callers are its loaded dependents plus the trusted set. Empty when
    // exempt or no enforce policy (fail-open); explicit policy overrides verbatim.
    std::vector<std::string> computeDerivedAllowedCallersLocked(const std::string& target) {
        if (std::find(kExemptTargets.begin(), kExemptTargets.end(), target)
                != kExemptTargets.end())
            return {};

        const auto& policy = parsedEnforcePolicy();
        if (!policy)
            return {};

        for (const auto& r : policy->restrictions)
            if (r.target == target)
                return r.allowedCallers;

        // Deduped; no dependents => trusted only (deny-by-default for peers).
        std::vector<std::string> callers;
        std::unordered_set<std::string> seen;
        auto add = [&](const std::string& c) {
            if (seen.insert(c).second)
                callers.push_back(c);
        };
        for (const auto& d : registryInstance().moduleDependents(target, /*recursive=*/false))
            if (registryInstance().isLoaded(d))
                add(d);
        for (const auto& t : kTrustedCallers)
            add(t);
        return callers;
    }

    void pushDerivedRestrictionForTarget(const std::string& target) {
        if (!registryInstance().isLoaded("capability_module"))
            return;
        auto callers = computeDerivedAllowedCallersLocked(target);
        if (!callers.empty())
            registerRestrictionRpc(target, callers);
    }

    // On load/unload of `name`, re-push the targets whose caller set changed:
    // its declared dependencies, plus `name` itself.
    void refreshDerivedRestrictionsForDependenciesOf(const std::string& name) {
        if (!registryInstance().isLoaded("capability_module"))
            return;
        for (const auto& dep : registryInstance().moduleDependencies(name, /*recursive=*/false))
            pushDerivedRestrictionForTarget(dep);
        pushDerivedRestrictionForTarget(name);
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

        // Build a descriptor for the loader to inspect.
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
        // calling load. The loader threads it through to the child via
        // a CLI argument so the child's LogosAPIProvider binds the right
        // listeners. Modules without an entry inherit the global default.
        if (auto it = moduleTransportsMap().find(name);
            it != moduleTransportsMap().end()) {
            desc.transportSetJson = it->second;
        }

        // ── Protocol-version load gate ─────────────────────────────────
        // Read the module's embedded metadata without loading it and apply
        // the one compatibility rule: equal logos-protocol MAJOR loads,
        // different MAJOR is refused, a missing stamp (pre-protocol module)
        // loads permissively with a warning.
        std::string moduleProtocolVersion;
        if (auto meta = ModuleLib::LogosModule::extractMetadata(modPath)) {
            // While we have it, hand the full metadata to the loader.
            desc.rawMetadata = nlohmann::json::parse(
                meta->rawMetadataJson, nullptr, /*allow_exceptions=*/false);
            if (desc.rawMetadata.is_discarded())
                desc.rawMetadata = nlohmann::json::object();
            if (auto it = desc.rawMetadata.find("logos_protocol_version");
                it != desc.rawMetadata.end() && it->is_string())
                moduleProtocolVersion = it->get<std::string>();
        }
        const auto gate = LogosCore::evaluateProtocolGate(
            moduleProtocolVersion, LOGOS_PROTOCOL_VERSION_MAJOR);
        switch (gate.decision) {
        case LogosCore::ProtocolGateDecision::Refuse:
            spdlog::error(
                "Refusing to load module {}: built against logos-protocol {} "
                "(major {}), this host speaks major {} ({}) — incompatible "
                "protocol majors",
                name, moduleProtocolVersion, gate.moduleMajor,
                LOGOS_PROTOCOL_VERSION_MAJOR, LOGOS_PROTOCOL_VERSION_STRING);
            return false;
        case LogosCore::ProtocolGateDecision::AllowLegacy:
            spdlog::warn(
                "Module {} carries no usable logos_protocol_version "
                "(pre-protocol build) — loading permissively",
                name);
            break;
        case LogosCore::ProtocolGateDecision::Allow:
            spdlog::debug("Module {} protocol version {} compatible with host {}",
                          name, moduleProtocolVersion,
                          LOGOS_PROTOCOL_VERSION_STRING);
            break;
        }

        auto loader = loaderRegistry().select(desc);
        if (!loader) {
            spdlog::warn("No loader available to load module: {}", name);
            return false;
        }

        // Fires on the loader's background (asio) thread when the module's
        // subprocess exits — but ONLY for unexpected exits: a deliberate
        // terminate() is suppressed upstream by the container's cancelled gate.
        // Keep the work here cheap and background-safe: markUnloaded (registry
        // has its own mutex) plus a non-blocking hop to the main thread for the
        // capability_module respawn. Do NO loadMutex / QtRO work here.
        auto onTerminated = [](const std::string& n) {
            registryInstance().markUnloaded(n);
            ModuleManager::maybeScheduleCapabilityRestart(n);
        };

        LogosCore::LoadedModuleHandle handle;
        if (!loader->load(desc, onTerminated, handle))
            return false;

        std::string authToken = boost::uuids::to_string(boost::uuids::random_generator()());

        if (!loader->sendToken(name, authToken)) {
            loader->terminate(name);
            return false;
        }

        registryInstance().markLoaded(name, loader, std::move(handle));

        TokenManager::instance().saveToken(name, authToken);

        notifyCapabilityModule(name, authToken);

        refreshDerivedRestrictionsForDependenciesOf(name);

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

        auto loader = registryInstance().loaderFor(name);
        if (loader) {
            if (!loader->hasModule(name)) {
                spdlog::warn("No module entry found for module: {}", name);
                return false;
            }
            loader->terminate(name);
        } else {
            // Fallback: module was loaded via markLoaded(name) directly (test
            // scenarios or external setup), so no loader was recorded. Ask the
            // registered loaders to terminate it by name — no specific container
            // is named here.
            if (!loaderRegistry().terminate(name)) {
                spdlog::warn("No live module entry found for module: {}", name);
                return false;
            }
        }

        registryInstance().markUnloaded(name);

        // markUnloaded keeps the dependency edges, so this still resolves them.
        refreshDerivedRestrictionsForDependenciesOf(name);

        spdlog::info("Module unloaded: {}", name);
        return true;
    }
}

namespace ModuleManager {

    ModuleRegistry& registry() {
        return registryInstance();
    }

    LogosCore::ModuleLoaderRegistry& loaders() {
        return loaderRegistry();
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
        // Cache the parse only in enforce mode; malformed/non-enforce stays empty.
        parsedEnforcePolicy().reset();
        if (!policyJson.empty()) {
            auto parsed = LogosCore::parseAccessPolicy(policyJson);
            if (!parsed) {
                spdlog::warn("logos_core_set_access_policy: policy is not valid JSON "
                             "— no restrictions will be enforced");
            } else if (parsed->enforce()) {
                parsedEnforcePolicy() = std::move(parsed);
            }
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

    // Load (or respawn) capability_module and re-establish ALL of its runtime
    // state. Assumes loadMutex() is held. One code path for both first boot and
    // crash recovery, so a respawn is guaranteed to end up in the same state as
    // a fresh start. Returns false if capability_module is unknown or fails to
    // load.
    bool reprovisionCapabilityLocked() {
        if (!registryInstance().isKnown("capability_module"))
            return false;

        if (!loadModuleInternal("capability_module")) {
            spdlog::warn("Failed to load capability module");
            return false;
        }

        // A respawned capability_module is a brand-new subprocess on the same
        // registry URL, so any cached core->capability client is latched onto
        // the dead socket. Drop it before we RPC to the fresh one.
        resetCapabilityModuleClient();

        // Register restrictions: explicit entries, then derived for anything
        // already loaded (on first boot only capability_module is up, so the
        // loop is a no-op; on a respawn every previously-loaded module is here).
        pushAccessRestrictionsToCapabilityModule();
        for (const auto& loaded : registryInstance().loadedModuleNames())
            pushDerivedRestrictionForTarget(loaded);

        // Re-teach every already-loaded module's token. A freshly respawned
        // capability_module starts with an EMPTY in-memory token store, so
        // without this the outage would survive the restart: every peer's
        // requestModule handshake keeps failing. loadModuleInternal above
        // already taught capability its own token; teach the rest here. No-op on
        // first boot. TokenManager is the durable source of truth (tokens are
        // saved there at each module's load), never capability's wiped map.
        for (const auto& name : registryInstance().loadedModuleNames()) {
            if (name == "capability_module")
                continue;
            notifyCapabilityModule(name, TokenManager::instance().getToken(name));
        }

        return true;
    }

    bool initializeCapabilityModule() {
        // A fresh start clears the shutdown latch so a prior clear()/cleanup in
        // the same process (tests, daemon restart) doesn't suppress recovery.
        shuttingDown().store(false);

        // Create the main-thread dispatcher used to marshal a crash-triggered
        // respawn off the loader's background thread. logos_core_start (our
        // caller) runs on the frontend's Qt thread, so this QObject gets the
        // right thread affinity. Harmless when there is no event loop (headless
        // tests): the queued respawn simply never runs.
        if (!restartDispatcher())
            restartDispatcher() = new QObject();

        std::lock_guard lock(loadMutex());
        return reprovisionCapabilityLocked();
    }

    // Sliding-window crash-loop guard. Touched only on the main thread inside
    // performCapabilityRestart, so no locking needed.
    std::deque<std::chrono::steady_clock::time_point>& capabilityRestartHistory() {
        static std::deque<std::chrono::steady_clock::time_point> h;
        return h;
    }

    // Runs on the main thread (posted from the crash hook). Respawns
    // capability_module, bounded by a crash-loop circuit breaker so a
    // deterministically-crashing capability can't fork-bomb.
    void performCapabilityRestart() {
        using namespace std::chrono;
        if (shuttingDown().load())
            return;

        constexpr int kMaxRestarts = 3;
        constexpr auto kWindow = seconds(60);

        auto& history = capabilityRestartHistory();
        const auto now = steady_clock::now();
        while (!history.empty() && now - history.front() > kWindow)
            history.pop_front();
        if (static_cast<int>(history.size()) >= kMaxRestarts) {
            spdlog::critical(
                "capability_module crashed {} times within {}s — auto-restart "
                "DISABLED, token exchange is DEGRADED; restart the application",
                kMaxRestarts, duration_cast<seconds>(kWindow).count());
            return;
        }
        history.push_back(now);

        std::lock_guard lock(loadMutex());
        // A concurrent explicit (re)load may have already brought it back.
        if (registryInstance().isLoaded("capability_module"))
            return;

        spdlog::warn("capability_module exited unexpectedly — auto-restarting "
                     "(attempt {} of {} within {}s)",
                     history.size(), kMaxRestarts,
                     duration_cast<seconds>(kWindow).count());
        if (reprovisionCapabilityLocked())
            spdlog::info("capability_module auto-restart succeeded; token "
                         "exchange restored");
        else
            spdlog::error("capability_module auto-restart failed");
    }

    // Runs on the loader's background (asio) thread from onTerminated. Decides
    // whether an exit warrants a respawn and, if so, hops it to the main thread.
    // Kept cheap and non-blocking: NO loadMutex, NO QtRO here.
    void maybeScheduleCapabilityRestart(const std::string& name) {
        if (!isAutoRestartTarget(name))
            return;
        if (shuttingDown().load())
            return;
        // Deliberate unloads never reach here (the container's cancelled gate
        // suppresses onTerminated for them), so any exit we see is a crash /
        // unexpected self-exit worth recovering from.

        QObject* dispatcher = restartDispatcher();
        if (!dispatcher || !QCoreApplication::instance()) {
            spdlog::error("capability_module exited but no event loop is "
                          "available to auto-restart it — token exchange is "
                          "DEGRADED until the application restarts");
            return;
        }

        // Non-blocking hop to the main thread. Must NOT block: this runs on the
        // shared loader io thread that also pumps every module's stdout.
        QMetaObject::invokeMethod(dispatcher, []() { performCapabilityRestart(); },
                                  Qt::QueuedConnection);
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
        // Suppress capability auto-restart: the terminations below are ours, not
        // crashes. (initializeCapabilityModule clears the latch on next boot.)
        shuttingDown().store(true);
        std::lock_guard lock(loadMutex());
        loaderRegistry().terminateAll();
        registryInstance().clearLoaded();
    }

    void clear() {
        shuttingDown().store(true);  // see terminateAll(): don't respawn during our own teardown
        std::lock_guard lock(loadMutex());
        loaderRegistry().terminateAll();
        registryInstance().clear();
        // Per-module transport overrides are part of the manager's
        // mutable state — without clearing them here, a daemon
        // restart in the same process (or a unit test that calls
        // clear() between scenarios) would inherit the previous
        // run's transport map and bind unexpected ports.
        moduleTransportsMap().clear();
        accessPolicyJson().clear();  // same rationale — don't leak across restarts
        parsedEnforcePolicy().reset();
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
        return loaderRegistry().getAllPids();
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

    std::string getModulesInfoJson() {
        return registryInstance().allModulesInfo().dump();
    }

    char* getModulesInfoCStr() {
        std::string json = getModulesInfoJson();
        char* result = new char[json.size() + 1];
        strcpy(result, json.c_str());
        return result;
    }

    std::vector<std::string> computeDerivedAllowedCallers(const std::string& target) {
        std::lock_guard lock(loadMutex());
        return computeDerivedAllowedCallersLocked(target);
    }
}
