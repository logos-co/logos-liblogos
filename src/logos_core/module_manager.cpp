#include "module_manager.h"
#include "module_registry.h"
#include "access_policy.h"
#include "dependency_resolver.h"
#include "module_loader_registry.h"
#include "composite_module_loader.h"
#include "module_state_observer.h"
#include <logos_container/container_factory.h>
#include <logos_module_loader/format_loader_factory.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <algorithm>
#include <mutex>
#include <cassert>
#include <cstring>
#include <optional>
#include <unordered_set>
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

    // ── Orderly teardown vs. death ───────────────────────────────────────────
    //
    // onTerminated fires for BOTH an unload we asked for and a module that
    // exited on its own, and cannot tell them apart from its arguments — but
    // they are different states (`stopping -> unloaded` vs `loaded -> error`).
    // So teardown announces intent here before terminate(); a name NOT in this
    // set died without being asked to.
    //
    // Its own mutex, not loadMutex(): the callback runs on the container's
    // background asio thread and must never wait behind a load in progress.
    std::mutex& expectedExitMutex() {
        static std::mutex m;
        return m;
    }

    std::unordered_set<std::string>& expectedExits() {
        static std::unordered_set<std::string> s;
        return s;
    }

    void markExitExpected(const std::string& name) {
        std::lock_guard<std::mutex> g(expectedExitMutex());
        expectedExits().insert(name);
    }

    // Consumes the mark: returns true exactly once per announced teardown, so a
    // module that is unloaded, reloaded and then CRASHES is reported as a crash
    // rather than inheriting the earlier orderly exit.
    bool consumeExpectedExit(const std::string& name) {
        std::lock_guard<std::mutex> g(expectedExitMutex());
        return expectedExits().erase(name) > 0;
    }

    // Host shutdown tears down EVERY loaded module at once, and each one
    // triggers onTerminated. Without announcing them first, a clean shutdown
    // reports the whole fleet as having crashed — `loaded -> error`, "module
    // exited without being asked to", once per module — which is both wrong and
    // the single most alarming thing this feed can say.
    //
    // Callers hold loadMutex(), so the loaded set cannot move underneath.
    void markAllLoadedExitsExpected() {
        for (const std::string& n : registryInstance().loadedModuleNames())
            markExitExpected(n);
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

        // INBOUND half of load-time identity: capability stores (name, token)
        // so authorize can name the caller from the presented token rather
        // than from a self-asserted fromModuleName.
        if (!client->informModuleToken(capabilityModuleToken, name, token)) {
            spdlog::warn("Failed to register token with capability module for: {}", name);
        }
    }

    // ── THE modules_state FEED ───────────────────────────────────────────────
    //
    // The consumer end of ModuleStateObserver. Follows the capability_module
    // precedent above — one long-lived "core" LogosAPI, per-module transport
    // honoured — with three differences forced by where it runs:
    //
    //   1. ASYNC. registerRestrictionRpc is synchronous and gets away with it
    //      because it is rare and short. This runs on EVERY load, unload and
    //      crash, from the observer's flush. A synchronous RPC there would put
    //      a 20 s worst case on the load path.
    //   2. CHEAP NO-OP WHEN ABSENT, checked before the client is even fetched.
    //      A synchronous dial to an absent module cost Basecamp ~417 s of
    //      blocked GUI thread once already.
    //   3. THE SINK IS UNINSTALLED when modules_state goes away, so the
    //      observer buffers nothing rather than buffering into a sink that
    //      only drops.

    constexpr const char* kModulesState = "modules_state";

    // An empty optional reaches the wire as JSON null: an invalid QVariant
    // falls through every branch of qvariantToNlohmann and returns nullptr.
    QVariant optToVariant(const std::optional<std::string>& v) {
        return v.has_value() ? QVariant(QString::fromStdString(*v)) : QVariant();
    }

    QVariant optToVariant(const std::optional<int64_t>& v) {
        return v.has_value() ? QVariant(static_cast<qlonglong>(*v)) : QVariant();
    }

    LogosAPIClient* modulesStateClient() {
        static LogosAPI* s_api = nullptr;
        if (!s_api)
            s_api = new LogosAPI(std::string("core"));

        if (auto it = moduleTransportsMap().find(kModulesState);
            it != moduleTransportsMap().end() && !it->second.empty()) {
            const auto ts = logos::transportSetFromJsonString(it->second);
            if (!ts.empty())
                return s_api->getClient(QString::fromUtf8(kModulesState), ts.front());
        }
        return s_api->getClient(std::string(kModulesState));
    }

    // Everything the host knows, as a ModuleListing, for apply_snapshot.
    //
    // THE SEQ RULE, the one thing here easy to get wrong: every record seq AND
    // the listing seq come from the observer's single counter, listing drawn
    // LAST so it is >= every record. modules_state tombstones a pruned record
    // at the LISTING's seq, so a second counter makes that tombstone
    // unreachably high or trivially low.
    nlohmann::json buildSnapshotListing() {
        auto& observer = logos::ModuleStateObserver::instance();
        nlohmann::json records = nlohmann::json::array();

        for (const auto& info : registryInstance().allModulesInfo()) {
            const std::string name = info.value("name", std::string());
            if (name.empty())
                continue;

            const bool loaded = info.value("loaded", false);

            nlohmann::json rec = nlohmann::json::object();
            rec["module"]       = name;
            rec["state"]        = loaded ? logos::module_state::kLoaded
                                         : logos::module_state::kUnloaded;
            rec["path"]         = info.value("path", std::string());
            rec["type"]         = std::string();
            rec["version"]      = std::string();
            rec["dependencies"] = info.value("dependencies", nlohmann::json::array());
            rec["dependents"]   = info.value("dependents", nlohmann::json::array());
            rec["loadedAt"]     = info.value("loaded_at", static_cast<int64_t>(0));
            rec["seq"]          = observer.nextSeq();
            // instance/pid/reason are OMITTED rather than nulled, matching the
            // generated encoder; the registry carries none of them.
            records.push_back(std::move(rec));
        }

        nlohmann::json listing = nlohmann::json::object();
        listing["modules"] = std::move(records);
        // FALSE, and it is a claim worth defending: `partial` means the scan
        // SKIPPED something, and discoverInstalledModules drops what it cannot
        // read before it ever enters the registry. Anything missing is not
        // withheld — it is unknown to the host, which is a complete view from
        // modules_state's side.
        listing["partial"] = false;
        listing["seq"]     = observer.nextSeq();
        return listing;
    }

    void pushSnapshot() {
        if (!registryInstance().isLoaded(kModulesState))
            return;
        nlohmann::json args = nlohmann::json::array();
        args.push_back(buildSnapshotListing());
        // Synchronous unlike the deltas, deliberately: this runs from
        // whenObjectAvailable's callback, not the load path, so no lock is held
        // and nothing waits on it. The nlohmann overload has no async twin, and
        // a struct argument is easier to build as JSON than as a QVariantMap.
        const nlohmann::json ok = modulesStateClient()->invokeRemoteMethod(
            std::string(kModulesState), std::string("apply_snapshot"), args);
        if (!ok.is_boolean() || !ok.get<bool>())
            spdlog::warn("modules_state refused the startup snapshot");
        else
            spdlog::info("Pushed module snapshot to modules_state");
    }

    void pushTransitions(const std::vector<logos::ModuleTransition>& batch) {
        // Before the client is fetched: see note 2 above.
        if (!registryInstance().isLoaded(kModulesState))
            return;

        LogosAPIClient* client = modulesStateClient();
        for (const logos::ModuleTransition& t : batch) {
            QVariantList args;
            args << QString::fromStdString(t.module)
                 << optToVariant(t.instance)
                 << optToVariant(t.pid)
                 << QString::fromStdString(t.oldState)
                 << QString::fromStdString(t.newState)
                 << optToVariant(t.reason)
                 << QVariant(static_cast<qulonglong>(t.seq));

            // Fire and forget: the next snapshot re-establishes the whole
            // picture, and blocking the load path to find out would be the
            // failure this shape exists to avoid.
            client->invokeRemoteMethodAsync(
                QString::fromUtf8(kModulesState),
                QStringLiteral("note_transition"),
                args,
                [](QVariant) {});
        }
    }

    // Called once modules_state is loaded. The snapshot waits for it to
    // PUBLISH, which is later than "loaded" (~390 ms cold);
    // whenObjectAvailable waits without failing fast or burning the acquire
    // timeout on this thread.
    void enableModulesStateFeed() {
        logos::ModuleStateObserver::instance().setSink(&pushTransitions);
        modulesStateClient()->whenObjectAvailable(
            QString::fromUtf8(kModulesState),
            [](bool ready) {
                if (ready)
                    pushSnapshot();
                else
                    spdlog::warn("modules_state never became available; no snapshot pushed");
            });
    }

    void disableModulesStateFeed() {
        // Clearing the sink is what makes the observer free again: record()
        // early-outs when nothing is installed.
        logos::ModuleStateObserver::instance().setSink({});
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

        // Hoisted out of the block below so the observer can carry it. The
        // instance id is the host's PERSISTENCE identity and is stable across
        // load/unload cycles (ResolveMode::ReuseOrCreate), which is exactly why
        // a consumer needs the pid too: instance cannot tell you a module died
        // and came back, and pid can.
        std::optional<std::string> instanceId;

        if (!persistenceBasePath().empty()) {
            auto info = ModuleLib::InstancePersistence::resolveInstance(
                persistenceBasePath(), name);
            desc.instancePersistencePath = info.persistencePath;
            if (!info.instanceId.empty())
                instanceId = info.instanceId;
        }

        // The attempt starts here — everything above was a cheap reject that
        // never touched the module. `loading` is the state the drafts fold into
        // `loaded`; we keep it because it is the only way a consumer can tell
        // "being brought up" from "up", and a load that hangs is otherwise
        // indistinguishable from one that never started.
        logos::ModuleStateObserver::instance().record(
            name, logos::module_state::kUnloaded, logos::module_state::kLoading,
            instanceId);

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
            logos::ModuleStateObserver::instance().record(
                name, logos::module_state::kLoading, logos::module_state::kError,
                instanceId, std::nullopt,
                "incompatible logos-protocol major: module " + moduleProtocolVersion);
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
            logos::ModuleStateObserver::instance().record(
                name, logos::module_state::kLoading, logos::module_state::kError,
                instanceId, std::nullopt, "no loader available for this module format");
            return false;
        }

        // Fires on the container's BACKGROUND asio thread, for both an orderly
        // unload and a module that died. consumeExpectedExit() is what tells
        // them apart; see its definition.
        //
        // This is the one seam that flushes inline: it is not under
        // loadMutex(), so there is no lock to get out from under, and a crash
        // is the transition a consumer most needs promptly.
        auto onTerminated = [](const std::string& n) {
            registryInstance().markUnloaded(n);

            auto& observer = logos::ModuleStateObserver::instance();
            if (consumeExpectedExit(n)) {
                observer.record(n, logos::module_state::kStopping,
                                logos::module_state::kUnloaded);
            } else {
                observer.record(n, logos::module_state::kLoaded,
                                logos::module_state::kError, std::nullopt,
                                std::nullopt, "module exited without being asked to");
            }
            observer.flush();
        };

        LogosCore::LoadedModuleHandle handle;
        if (!loader->load(desc, onTerminated, handle)) {
            logos::ModuleStateObserver::instance().record(
                name, logos::module_state::kLoading, logos::module_state::kError,
                instanceId, std::nullopt, "loader failed to start the module");
            return false;
        }

        // Read before the handle is moved into the registry below.
        const std::optional<int64_t> pid =
            handle.pid >= 0 ? std::optional<int64_t>(handle.pid) : std::nullopt;

        // OUTBOUND half of load-time identity: mint a root token, send it into
        // the child, and register it locally under the module's name.
        std::string authToken = boost::uuids::to_string(boost::uuids::random_generator()());

        if (!loader->sendToken(name, authToken)) {
            // We are about to terminate it deliberately, so announce the intent
            // BEFORE calling terminate() — otherwise onTerminated, which may
            // already be running on the asio thread, reports this as a crash.
            markExitExpected(name);
            loader->terminate(name);
            logos::ModuleStateObserver::instance().record(
                name, logos::module_state::kLoading, logos::module_state::kError,
                instanceId, pid, "failed to deliver the module's auth token");
            return false;
        }

        registryInstance().markLoaded(name, loader, std::move(handle));

        TokenManager::instance().saveToken(name, authToken);

        notifyCapabilityModule(name, authToken);

        refreshDerivedRestrictionsForDependenciesOf(name);

        spdlog::info("Module loaded: {}", name);
        logos::ModuleStateObserver::instance().record(
            name, logos::module_state::kLoading, logos::module_state::kLoaded,
            instanceId, pid);

        // The feed can only exist once its consumer does.
        if (name == kModulesState)
            enableModulesStateFeed();

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

        logos::ModuleStateObserver::instance().record(
            name, logos::module_state::kLoaded, logos::module_state::kStopping);

        auto loader = registryInstance().loaderFor(name);
        if (loader) {
            if (!loader->hasModule(name)) {
                spdlog::warn("No module entry found for module: {}", name);
                // Nothing was torn down, so the module is still where it was.
                // Walk `stopping` back rather than leaving a consumer watching
                // a teardown that never happens.
                logos::ModuleStateObserver::instance().record(
                    name, logos::module_state::kStopping, logos::module_state::kLoaded,
                    std::nullopt, std::nullopt, "no module entry found; teardown not started");
                return false;
            }
            // Announce BEFORE terminate(): onTerminated can fire on the asio
            // thread before terminate() even returns here.
            markExitExpected(name);
            loader->terminate(name);
        } else {
            // Fallback: module was loaded via markLoaded(name) directly (test
            // scenarios or external setup), so no loader was recorded. Ask the
            // registered loaders to terminate it by name — no specific container
            // is named here.
            markExitExpected(name);
            if (!loaderRegistry().terminate(name)) {
                spdlog::warn("No live module entry found for module: {}", name);
                consumeExpectedExit(name);
                logos::ModuleStateObserver::instance().record(
                    name, logos::module_state::kStopping, logos::module_state::kLoaded,
                    std::nullopt, std::nullopt, "no live module entry; teardown not started");
                return false;
            }
        }

        registryInstance().markUnloaded(name);

        // markUnloaded keeps the dependency edges, so this still resolves them.
        refreshDerivedRestrictionsForDependenciesOf(name);

        spdlog::info("Module unloaded: {}", name);

        // THE MARK IS THE GUARD: the observer is stateless, so it cannot spot
        // a duplicate `stopping -> unloaded` the way it drops old == new, and
        // emitting twice would reach modules_state as two transitions with
        // different seqs — two identical events for one teardown. If
        // onTerminated already ran it consumed the mark; if the mark is still
        // here no callback fired (loaders that terminate synchronously) and
        // without this the module sits in `stopping` forever.
        if (consumeExpectedExit(name)) {
            logos::ModuleStateObserver::instance().record(
                name, logos::module_state::kStopping, logos::module_state::kUnloaded);
        }
        if (name == kModulesState)
            disableModulesStateFeed();
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

    // THE deny-by-default switch. `mode: "enforce"` is the whole flag: it is
    // what turns the derived restrictions on (computeDerivedAllowedCallersLocked
    // returns {} without it, so core registers nothing and capability_module
    // leaves every target open). Anything else — no policy, empty policy,
    // unparseable policy, a different mode — is OFF, i.e. exactly the behaviour
    // of a host that never calls this at all.
    //
    // Every branch says out loud which side it landed on. Enforcement that
    // silently failed to arm is the dangerous outcome: it looks identical to
    // enforcement that is working and simply has nothing to deny, so an
    // operator who mistyped `"mode":"enforced"` would otherwise get a
    // wide-open runtime and a clean log.
    void setAccessPolicy(const std::string& policyJson) {
        std::lock_guard<std::mutex> g(loadMutex());  // guards the read at push time
        accessPolicyJson() = policyJson;
        // Cache the parse only in enforce mode; malformed/non-enforce stays empty.
        parsedEnforcePolicy().reset();

        if (policyJson.empty()) {
            spdlog::info("Inter-module access enforcement is OFF (no access policy set): "
                         "any loaded module may call any other");
            return;
        }

        auto parsed = LogosCore::parseAccessPolicy(policyJson);
        if (!parsed) {
            spdlog::warn("logos_core_set_access_policy: policy is not valid JSON — "
                         "inter-module access enforcement stays OFF");
            return;
        }
        if (!parsed->enforce()) {
            spdlog::warn("logos_core_set_access_policy: mode is \"{}\", not \"enforce\" — "
                         "inter-module access enforcement stays OFF ({} restriction(s) "
                         "parsed but not registered)",
                         parsed->mode, parsed->restrictions.size());
            return;
        }

        spdlog::info("Inter-module access enforcement is ON (mode=enforce): deny-by-default — "
                     "a module may only call the modules it declares as dependencies; "
                     "{} explicit restriction(s) override the derived allow-list",
                     parsed->restrictions.size());
        parsedEnforcePolicy() = std::move(parsed);
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
        // BEFORE the lock guard, so it is destroyed after it. See rule 1.
        logos::ScopedModuleStateFlush stateFlusher;
        std::lock_guard lock(loadMutex());
        return loadModuleInternal(moduleName);
    }

    bool loadModuleWithDependencies(const char* moduleName) {
        // BEFORE the lock guard, so it is destroyed after it. See rule 1.
        logos::ScopedModuleStateFlush stateFlusher;
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
        // BEFORE the lock guard, so it is destroyed after it. See rule 1.
        logos::ScopedModuleStateFlush stateFlusher;
        std::lock_guard lock(loadMutex());

        if (!registryInstance().isKnown("capability_module"))
            return false;

        if (!loadModuleInternal("capability_module")) {
            spdlog::warn("Failed to load capability module");
            return false;
        }

        // Register restrictions before any other module can call out: explicit
        // entries, then derived for anything already loaded (usually nothing —
        // only the exempt capability_module is up here).
        pushAccessRestrictionsToCapabilityModule();
        for (const auto& loaded : registryInstance().loadedModuleNames())
            pushDerivedRestrictionForTarget(loaded);

        return true;
    }

    bool unloadModule(const char* moduleName) {
        // BEFORE the lock guard, so it is destroyed after it. See rule 1.
        logos::ScopedModuleStateFlush stateFlusher;
        std::lock_guard lock(loadMutex());
        return unloadModuleInternalLocked(std::string(moduleName));
    }

    bool unloadModuleWithDependents(const char* moduleName) {
        // BEFORE the lock guard, so it is destroyed after it. See rule 1.
        logos::ScopedModuleStateFlush stateFlusher;
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
        // BEFORE the lock guard, so it is destroyed after it. See rule 1.
        logos::ScopedModuleStateFlush stateFlusher;
        std::lock_guard lock(loadMutex());
        // Announce before tearing down, or every module reports as a crash.
        markAllLoadedExitsExpected();
        loaderRegistry().terminateAll();
        registryInstance().clearLoaded();
    }

    void clear() {
        // BEFORE the lock guard, so it is destroyed after it. See rule 1.
        logos::ScopedModuleStateFlush stateFlusher;
        std::lock_guard lock(loadMutex());
        // Announce before tearing down, or every module reports as a crash.
        markAllLoadedExitsExpected();
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
