#include "embedded_core_service.h"
#include "call_envelope.h"

#include "bootstrap_policy.h"
#include "token_authority.h"
#include "logos_core.h"
#include "module_manager.h"
#include "module_registry.h"
#include "module_state_observer.h"

#include <logos_protocol.h>
#include <nlohmann/json.hpp>
#include <process_stats/process_stats.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace logos::core_service {
namespace {

using json = nlohmann::json;

constexpr const char* kName = "core_service";
constexpr int kModuleCallMs = 20000;
constexpr unsigned kConcurrentCalls = 8;

struct Config {
    std::mutex mutex;
    std::string transports;
    ShutdownHandler shutdown = nullptr;
    void* shutdownData = nullptr;
    OperatorResolver operators = nullptr;
    void* operatorData = nullptr;
    Extension extension = nullptr;
    json extensionMethods = json::array();
    void* extensionData = nullptr;
    std::string shell;
};

Config& config()
{
    static Config value;
    return value;
}

struct Watch {
    lp_subscription* subscription = nullptr;
    std::string* module = nullptr; // forwardEvent's context
};

struct Service {
    std::mutex mutex;
    lp_provider* provider = nullptr;
    int sink = 0;
    std::map<std::pair<std::string, std::string>, lp_client*> clients; // (origin, target)
    // One forwarder per module and event ("" is every event).
    std::map<std::string, std::map<std::string, Watch>> watches;
    std::set<std::string> consumers; // admitted here, so retireConsumer may end them
};

// Outside the lock: an unsubscribe waits for a forward in flight.
void release(std::vector<Watch>& watches)
{
    for (Watch& watch : watches) {
        lp_unsubscribe(watch.subscription);
        delete watch.module;
    }
    watches.clear();
}

Service& service()
{
    static Service value;
    return value;
}

// ── who is calling ────────────────────────────────────────────────────────────

struct Caller {
    std::string kind;
    std::string name;
};

Caller currentCaller()
{
    const char* text = lp_current_caller_json();
    const json doc = json::parse(text ? text : "{}", nullptr, false);
    if (!doc.is_object()) return {"unknown", {}};
    return {doc.value("kind", std::string{"unknown"}), doc.value("name", std::string{})};
}

enum class Scope { Read, Control, Shell, Stop, Forward };

std::optional<Scope> scopeOf(const std::string& method)
{
    static const std::map<std::string, Scope> scopes = {
        {"listModules", Scope::Read},      {"getStatus", Scope::Read},
        {"getModuleInfo", Scope::Read},    {"getModuleStats", Scope::Read},
        {"getModulesInfo", Scope::Read},   {"getModuleDependencies", Scope::Read},
        {"getModuleDependents", Scope::Read}, {"getModuleOptionalDependencies", Scope::Read},
        {"getOptionalLoadReport", Scope::Read},
        {"loadModule", Scope::Control},    {"unloadModule", Scope::Control},
        {"reloadModule", Scope::Control},  {"refreshModules", Scope::Control},
        {"admitConsumer", Scope::Shell},   {"retireConsumer", Scope::Shell},
        {"shutdown", Scope::Stop},
        {"callModuleMethod", Scope::Forward}, {"watchModuleEvents", Scope::Forward},
    };
    auto it = scopes.find(method);
    return it == scopes.end() ? std::nullopt : std::optional<Scope>(it->second);
}

bool isShell(const Caller& caller)
{
    std::lock_guard<std::mutex> lock(config().mutex);
    return caller.kind == "module" && !config().shell.empty() && caller.name == config().shell;
}

// The runtime (host) may do anything; unknown callers nothing.
bool allowed(const Caller& caller, Scope scope)
{
    if (caller.kind == "host") return true;
    const bool operatorCaller = caller.kind == "operator" && !caller.name.empty();
    const bool admitted = (caller.kind == "module" && !caller.name.empty()) || operatorCaller;
    switch (scope) {
    case Scope::Read: return admitted;
    case Scope::Control: return isShell(caller) || operatorCaller;
    case Scope::Shell: return isShell(caller);
    case Scope::Stop: return isShell(caller) || operatorCaller;
    case Scope::Forward: return operatorCaller;
    }
    return false;
}

json error(const std::string& code, const std::string& message)
{
    return {{"status", "error"}, {"code", code}, {"message", message}};
}

// ── the runtime, straight from its module manager ────────────────────────────

std::vector<std::string> loadedNames() { return ModuleManager::registry().loadedModuleNames(); }
std::vector<std::string> knownNames() { return ModuleManager::registry().knownModuleNames(); }

bool contains(const std::vector<std::string>& list, const std::string& name)
{
    return std::find(list.begin(), list.end(), name) != list.end();
}

json modulesInfo()
{
    const json info = json::parse(ModuleManager::getModulesInfoJson(), nullptr, false);
    return info.is_array() ? info : json::array();
}

std::string versionOf(const json& entry)
{
    const auto meta = entry.find("metadata");
    return meta != entry.end() && meta->is_object() ? meta->value("version", std::string{})
                                                   : std::string{};
}

int64_t uptimeOf(const json& entry)
{
    if (!entry.value("loaded", false)) return -1;
    const int64_t loadedAt = entry.value("loaded_at", int64_t{0});
    if (loadedAt <= 0) return -1;
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::max<int64_t>(0, now - loadedAt);
}

lp_client* clientFor(const std::string& origin, const std::string& target)
{
    Service& s = service();
    std::lock_guard<std::mutex> lock(s.mutex);
    lp_client*& slot = s.clients[{origin, target}];
    if (!slot) slot = lp_client_create(target.c_str(), origin.c_str(), nullptr, nullptr);
    return slot;
}

json invoke(lp_client* client, const std::string& target, const std::string& method,
            const json& args, CallFailure* failure)
{
    char* result = nullptr;
    char* err = nullptr;
    const int status = lp_invoke(client, method.c_str(), args.dump().c_str(), kModuleCallMs,
                                 &result, &err);
    json value = nullptr;
    if (status == LP_OK && result) {
        value = json::parse(result, nullptr, false);
        if (value.is_discarded()) value = nullptr;
    } else if (failure) {
        const json detail = err ? json::parse(err, nullptr, false) : json();
        const bool object = detail.is_object();
        *failure = {object ? detail.value("code", std::string{"call_failed"}) : "call_failed",
                    object ? detail.value("message", std::string{"the call failed"})
                           : std::string("the call failed"),
                    object ? detail.value("origin", target) : target};
    }
    lp_string_free(result);
    lp_string_free(err);
    return value;
}

std::vector<std::string> methodNames(lp_client* client)
{
    std::vector<std::string> result;
    char* text = lp_get_methods(client);
    const json methods = text ? json::parse(text, nullptr, false) : json();
    lp_string_free(text);
    if (!methods.is_array()) return result;
    for (const auto& m : methods) {
        if (m.is_object() && m.contains("name") && m["name"].is_string())
            result.push_back(m["name"].get<std::string>());
        else if (m.is_string())
            result.push_back(m.get<std::string>());
    }
    return result;
}

void emit(const std::string& event, const json& data)
{
    lp_provider* provider = nullptr;
    {
        std::lock_guard<std::mutex> lock(service().mutex);
        provider = service().provider;
    }
    if (provider) (void)lp_provider_emit_event(provider, event.c_str(), data.dump().c_str());
}

// ── the methods (the shapes logosctl reads) ───────────────────────────────────

// Ensure-loaded: a module already up answers ok. `deps` is how much of the graph
// comes too; logosctl leaves it at everything installed.
bool load(const std::string& name, const std::string& deps)
{
    if (deps == "module_only") return ModuleManager::loadModule(name.c_str());
    return ModuleManager::loadModuleWithDependencies(
        name.c_str(), deps == "required" ? DependencyResolver::OptionalLoad::OrderOnly
                                         : DependencyResolver::OptionalLoad::BestEffort);
}

// The token authority stops only with the runtime.
json refuseAuthority(const std::string& name)
{
    return error("FORBIDDEN", "'" + name + "' is the token authority; it stops only with the runtime.");
}

json loadModule(const std::string& name, const std::string& deps)
{
    if (deps != "module_only" && deps != "required" && deps != "required_and_optional")
        return error("INVALID_ARGS", "deps is module_only, required or required_and_optional");
    const std::vector<std::string> before = loadedNames();
    if (!load(name, deps)) {
        json result = error("MODULE_LOAD_FAILED", "Failed to load module '" + name + "'.");
        result["known_modules"] = knownNames();
        return result;
    }
    const std::unordered_set<std::string> earlier(before.begin(), before.end());
    json dependencies = json::array();
    for (const auto& loaded : loadedNames())
        if (loaded != name && !earlier.count(loaded)) dependencies.push_back(loaded);
    std::string version;
    for (const auto& entry : modulesInfo())
        if (entry.value("name", std::string{}) == name) version = versionOf(entry);
    json result = {{"status", "ok"}, {"module", name}, {"version", version},
                   {"dependencies_loaded", dependencies}};
    const json skipped = json::parse(ModuleManager::optionalLoadReportJson(name), nullptr, false);
    if (skipped.is_array() && !skipped.empty()) result["optional_skipped"] = skipped;
    return result;
}

json unloadModule(const std::string& name, bool withDependents)
{
    if (name == "capability_module") return refuseAuthority(name);
    const std::vector<std::string> before = loadedNames();
    if (withDependents) ModuleManager::unloadModuleWithDependents(name.c_str());
    else ModuleManager::unloadModule(name.c_str());
    const std::vector<std::string> after = loadedNames();
    if (contains(after, name))
        return error("MODULE_NOT_LOADED", "Module '" + name + "' is not loaded.");
    json dependents = json::array();
    for (const auto& was : before)
        if (was != name && !contains(after, was)) dependents.push_back(was);
    return {{"status", "ok"}, {"module", name}, {"dependents_unloaded", dependents}};
}

json reloadModule(const std::string& name)
{
    if (name == "capability_module") return refuseAuthority(name);
    const bool wasLoaded = contains(loadedNames(), name);
    json result = {{"action", "reload"}, {"module", name},
                   {"previous_status", wasLoaded ? "loaded" : "not_loaded"}};
    if (wasLoaded) ModuleManager::unloadModule(name.c_str());
    if (!load(name, "required_and_optional")) {
        result["status"] = "error";
        if (wasLoaded) {
            const bool restored = load(name, "required_and_optional");
            result["error"] = restored ? "reload failed; previous instance restored"
                                       : "reload failed; module is now unloaded";
            result["restored"] = restored;
        } else {
            result["error"] = "module failed to start";
        }
        return result;
    }
    result["status"] = "loaded";
    for (const auto& entry : modulesInfo())
        if (entry.value("name", std::string{}) == name) result["version"] = versionOf(entry);
    return result;
}

json refreshModules()
{
    ModuleManager::discoverInstalledModules();
    return {{"status", "ok"}, {"known_modules", knownNames()}};
}

json listModules(const std::string& filter)
{
    json modules = json::array();
    for (const auto& entry : modulesInfo()) {
        const bool loaded = entry.value("loaded", false);
        if (filter == "loaded" && !loaded) continue;
        json module = {{"name", entry.value("name", std::string{})},
                       {"status", loaded ? "loaded" : "not_loaded"},
                       {"version", versionOf(entry)}};
        if (const int64_t up = uptimeOf(entry); up >= 0) module["uptime_seconds"] = up;
        modules.push_back(std::move(module));
    }
    return modules;
}

json getStatus()
{
#ifdef _WIN32
    const int64_t pid = ::_getpid();
#else
    const int64_t pid = ::getpid();
#endif
    const json modules = listModules("all");
    int loaded = 0, notLoaded = 0;
    for (const auto& m : modules) (m.value("status", std::string{}) == "loaded" ? loaded : notLoaded)++;
    return {{"daemon", {{"status", "running"}, {"pid", pid}, {"version", "1.0.0"}}},
            {"modules", modules},
            {"modules_summary", {{"loaded", loaded}, {"crashed", 0}, {"not_loaded", notLoaded}}}};
}

json getModuleInfo(const std::string& name)
{
    json entry;
    for (const auto& e : modulesInfo())
        if (e.value("name", std::string{}) == name) entry = e;
    if (entry.is_null()) return error("MODULE_NOT_FOUND", "Module '" + name + "' not found.");
    json info = {{"name", name}, {"version", versionOf(entry)},
                 {"dependencies", entry.value("dependencies", json::array())},
                 {"dependents", entry.value("dependents", json::array())}};
    if (!entry.value("loaded", false) || name == kName) {
        info["status"] = entry.value("loaded", false) ? "loaded" : "not_loaded";
        return info;
    }
    info["status"] = "loaded";
    info["placement"] = entry.value("placement", json(nullptr));
    if (const int64_t up = uptimeOf(entry); up >= 0) info["uptime_seconds"] = up;
    if (lp_client* client = clientFor("core", name)) {
        char* text = lp_get_methods(client);
        const json methods = text ? json::parse(text, nullptr, false) : json();
        lp_string_free(text);
        if (methods.is_array()) info["methods"] = methods;
        const json events = invoke(client, name, "getPluginEvents", json::array(), nullptr);
        if (events.is_array()) info["events"] = events;
    }
    return info;
}

json getModuleStats()
{
    char* text = ProcessStats::getModuleStats(ModuleManager::getModuleProcessIds());
    json stats = text ? json::parse(text, nullptr, false) : json::array();
    delete[] text;
    return stats.is_discarded() ? json::array() : stats;
}

// The graph and the metadata, as the C API answered them before it moved here.
json getModuleDependencies(const std::string& name, bool recursive)
{
    return ModuleManager::getDependencies(name, recursive);
}

json getModuleDependents(const std::string& name, bool recursive)
{
    return ModuleManager::getDependents(name, recursive);
}

json getModuleOptionalDependencies(const std::string& name)
{
    return ModuleManager::getOptionalDependencies(name);
}

json getOptionalLoadReport(const std::string& name)
{
    const json report = json::parse(ModuleManager::optionalLoadReportJson(name), nullptr, false);
    return report.is_array() ? report : json::array();
}

// Package modules take settings from the runtime only, so an operator's package
// commands reach them as this service, which admitted the operator.
bool isPackageModule(const std::string& module)
{
    return module == "package_manager" || module == "package_downloader";
}

// An operator's call reaches the target as that operator, never as the runtime,
// and never reaches the token store or this service.
// The token store and core_service itself are never an operator's target.
bool closedToOperator(const Caller& caller, const std::string& module)
{
    return caller.kind == "operator" && (module == "capability_module" || module == kName);
}

json callModuleMethod(const Caller& caller, const std::string& module, const std::string& method,
                      const json& args)
{
    if (module != kName && !contains(loadedNames(), module))
        return error("MODULE_NOT_LOADED", "Module '" + module + "' is not loaded. Load it with: "
                                          "logosctl module load " + module);
    // Refused like any unauthorized call, so the answer is the usual envelope.
    if (closedToOperator(caller, module))
        return callEnvelope(module, method, nullptr,
                            CallFailure{"unauthorized",
                                        "an operator cannot call " + module + " through core_service",
                                        kName},
                            {});
    // Only the runtime itself calls as the runtime; an operator goes as itself.
    std::string origin = "core";
    if (caller.kind == "operator") {
        if (!authority::attached())
            return error("UNAVAILABLE", "no token authority is running");
        if (isPackageModule(module)) {
            origin = kName;
        } else {
            const std::string pair = authority::grantOperatorPair(caller.name, module);
            if (pair.empty())
                return error("FORBIDDEN",
                             "No token for operator '" + caller.name + "' at '" + module + "'.");
            origin = "@op:" + caller.name;
            lp_token_isolate_identity(origin.c_str());
            lp_token_save_for(origin.c_str(), module.c_str(), pair.c_str());
        }
    }
    lp_client* client = clientFor(origin, module);
    if (!client)
        return error("INTERNAL_ERROR", "Could not obtain a client for loaded module '" + module + "'.");
    CallFailure failure;
    const json returned = invoke(client, module, method, args, &failure);
    return callEnvelope(module, method, returned, failure,
                        [client] { return methodNames(client); });
}

void forwardEvent(const char* event, const char* data, void* userData)
{
    const auto* module = static_cast<const std::string*>(userData);
    json forwarded = json::array({*module, event ? event : ""});
    const json payload = data ? json::parse(data, nullptr, false) : json::array();
    if (payload.is_array())
        for (const auto& value : payload) forwarded.push_back(value);
    emit("module_event", forwarded);
}

// Every forwarder broadcasts module_event to all subscribers, so a second one for
// the same event would deliver it twice. A watch on every event covers the rest.
json watchModuleEvents(const Caller& caller, const std::string& module, const std::string& event)
{
    if (closedToOperator(caller, module) || !contains(loadedNames(), module)) return false;
    auto covered = [&](const std::map<std::string, Watch>& events) {
        return events.count("") > 0 || events.count(event) > 0;
    };
    {
        std::lock_guard<std::mutex> lock(service().mutex);
        const auto it = service().watches.find(module);
        if (it != service().watches.end() && covered(it->second)) return true;
    }
    lp_client* client = clientFor("core", module);
    if (!client) return false;
    auto* context = new std::string(module);
    lp_subscription* subscription = lp_subscribe(client, event.c_str(), &forwardEvent, context);
    if (!subscription) {
        delete context;
        return false;
    }
    std::vector<Watch> surplus;
    {
        std::lock_guard<std::mutex> lock(service().mutex);
        auto& events = service().watches[module];
        if (covered(events)) {
            surplus.push_back({subscription, context}); // an equal watch got here first
        } else {
            if (event.empty()) {
                for (auto& [name, watch] : events) surplus.push_back(watch);
                events.clear();
            }
            events[event] = {subscription, context};
        }
    }
    release(surplus);
    // Unloaded while subscribing: its transition may already have passed.
    if (!contains(loadedNames(), module)) {
        std::vector<Watch> ended;
        {
            std::lock_guard<std::mutex> lock(service().mutex);
            const auto it = service().watches.find(module);
            if (it != service().watches.end()) {
                for (auto& [name, watch] : it->second) ended.push_back(watch);
                service().watches.erase(it);
            }
        }
        release(ended);
        return false;
    }
    return true;
}

// A module's name is never a consumer's: admitting it would retire the module.
json admitConsumer(const std::string& name, const std::string& kind)
{
    if (!logos::isValidModuleName(name) || bootstrap::isReservedName(name)
        || contains(knownNames(), name))
        return error("INVALID_ARGS", "'" + name + "' cannot be admitted as a consumer.");
    if (kind != "presentation") return error("INVALID_ARGS", "only presentation consumers");
    if (!authority::attached()) return error("UNAVAILABLE", "no token authority is running");
    const std::string credential = authority::admit(name, kind);
    if (credential.empty()) return error("FORBIDDEN", "capability_module refused '" + name + "'.");
    {
        std::lock_guard<std::mutex> lock(service().mutex);
        service().consumers.insert(name);
    }
    return {{"status", "ok"}, {"name", name}, {"credential", credential}};
}

json retireConsumer(const std::string& name)
{
    {
        std::lock_guard<std::mutex> lock(service().mutex);
        if (!service().consumers.erase(name))
            return error("NOT_FOUND", "'" + name + "' is not a consumer admitted here.");
    }
    authority::retire(name);
    return {{"status", "ok"}, {"name", name}};
}

json shutdownRuntime()
{
    ShutdownHandler handler = nullptr;
    void* data = nullptr;
    {
        std::lock_guard<std::mutex> lock(config().mutex);
        handler = config().shutdown;
        data = config().shutdownData;
    }
    if (!handler) return error("UNSUPPORTED", "This runtime's embedder takes no shutdown request.");
    handler(data);
    return {{"status", "ok"}, {"message", "Daemon shutting down."}};
}

json run(const std::string& method, const json& args, const Caller& caller)
{
    auto text = [&](std::size_t i, const char* fallback = nullptr) -> std::string {
        if (i < args.size()) return args[i].get<std::string>();
        if (fallback) return fallback;
        throw std::invalid_argument("missing argument " + std::to_string(i + 1));
    };
    if (method == "loadModule") return loadModule(text(0), text(1, "required_and_optional"));
    if (method == "unloadModule")
        return unloadModule(text(0), args.size() >= 2 ? args[1].get<bool>() : true);
    if (method == "reloadModule") return reloadModule(text(0));
    if (method == "refreshModules") return refreshModules();
    if (method == "listModules") return listModules(text(0, "all"));
    if (method == "getStatus") return getStatus();
    if (method == "getModuleInfo") return getModuleInfo(text(0));
    if (method == "getModuleStats") return getModuleStats();
    auto flag = [&](std::size_t i) { return i < args.size() && args[i].get<bool>(); };
    if (method == "getModulesInfo") return modulesInfo();
    if (method == "getModuleDependencies") return getModuleDependencies(text(0), flag(1));
    if (method == "getModuleDependents") return getModuleDependents(text(0), flag(1));
    if (method == "getModuleOptionalDependencies") return getModuleOptionalDependencies(text(0));
    if (method == "getOptionalLoadReport") return getOptionalLoadReport(text(0));
    if (method == "callModuleMethod")
        return callModuleMethod(caller, text(0), text(1),
                                args.size() >= 3 && args[2].is_array() ? args[2] : json::array());
    if (method == "watchModuleEvents") return watchModuleEvents(caller, text(0), text(1, ""));
    if (method == "admitConsumer") return admitConsumer(text(0), text(1, "presentation"));
    if (method == "retireConsumer") return retireConsumer(text(0));
    if (method == "shutdown") return shutdownRuntime();
    return nullptr;
}

// ── the provider ──────────────────────────────────────────────────────────────

char* copy(const json& value)
{
    return lp_string_copy(value.dump().c_str());
}

char* dispatch(const char* method, const char* argsJson, void*)
{
    const std::string name = method ? method : "";
    const Caller caller = currentCaller();
    const std::optional<Scope> scope = scopeOf(name);
    if (!scope) {
        Extension extension = nullptr;
        void* data = nullptr;
        {
            std::lock_guard<std::mutex> lock(config().mutex);
            extension = config().extension;
            data = config().extensionData;
        }
        if (!extension) return nullptr;
        const char* callerJson = lp_current_caller_json();
        return extension(callerJson ? callerJson : "{}", name.c_str(),
                         argsJson && *argsJson ? argsJson : "[]", data);
    }
    if (!allowed(caller, *scope))
        return copy(error("FORBIDDEN", "core_service." + name + " is not open to "
                                       + (caller.name.empty() ? caller.kind : caller.name) + "."));
    const json args = json::parse(argsJson && *argsJson ? argsJson : "[]", nullptr, false);
    if (!args.is_array()) return copy(error("INVALID_ARGS", "arguments must be an array"));
    try {
        return copy(run(name, args, caller));
    } catch (const std::exception& e) {
        return copy(error("INVALID_ARGS", std::string("invalid arguments: ") + e.what()));
    }
}

char* methods(void*)
{
    json list = json::array();
    auto add = [&](const char* name, std::vector<std::pair<const char*, const char*>> params,
                   const char* returns) {
        json parameters = json::array();
        for (const auto& [p, type] : params) parameters.push_back({{"name", p}, {"type", type}});
        list.push_back({{"type", "method"}, {"name", name}, {"returnType", returns},
                        {"isInvokable", true}, {"parameters", parameters}});
    };
    add("loadModule", {{"name", "string"}, {"deps", "string"}}, "StdLogosResult");
    add("unloadModule", {{"name", "string"}, {"withDependents", "bool"}}, "StdLogosResult");
    add("reloadModule", {{"name", "string"}}, "StdLogosResult");
    add("refreshModules", {}, "LogosMap");
    add("listModules", {{"filter", "string"}}, "LogosList");
    add("getStatus", {}, "LogosMap");
    add("getModuleInfo", {{"name", "string"}}, "LogosMap");
    add("getModuleStats", {}, "LogosList");
    add("getModulesInfo", {}, "LogosList");
    add("getModuleDependencies", {{"name", "string"}, {"recursive", "bool"}}, "LogosList");
    add("getModuleDependents", {{"name", "string"}, {"recursive", "bool"}}, "LogosList");
    add("getModuleOptionalDependencies", {{"name", "string"}}, "LogosList");
    add("getOptionalLoadReport", {{"name", "string"}}, "LogosList");
    add("callModuleMethod", {{"module", "string"}, {"method", "string"}, {"args", "LogosList"}},
        "StdLogosResult");
    add("watchModuleEvents", {{"module", "string"}, {"eventName", "string"}}, "bool");
    add("admitConsumer", {{"name", "string"}, {"kind", "string"}}, "LogosMap");
    add("retireConsumer", {{"name", "string"}}, "LogosMap");
    add("shutdown", {}, "LogosMap");
    {
        std::lock_guard<std::mutex> lock(config().mutex);
        for (const auto& extra : config().extensionMethods) list.push_back(extra);
    }
    return copy(list);
}

// Callers the provider's own table does not know: capability's admissions,
// then the embedder's operators.
char* resolveCaller(const char* token, const char* transport, void*)
{
    if (auto document = authority::resolveCaller(token, transport))
        return lp_string_copy(document->c_str());
    OperatorResolver resolver = nullptr;
    void* data = nullptr;
    {
        std::lock_guard<std::mutex> lock(config().mutex);
        resolver = config().operators;
        data = config().operatorData;
    }
    if (!resolver || !token) return nullptr;
    char* name = resolver(token, transport ? transport : "", data);
    const std::string op = name ? name : "";
    lp_string_free(name);
    if (op.empty()) return nullptr;
    return copy(json{{"kind", "operator"}, {"name", op}});
}

// A module that leaves ends its watches; one asked for after it returns subscribes afresh.
void publishTransitions(const std::vector<ModuleTransition>& batch)
{
    std::vector<Watch> ended;
    {
        std::lock_guard<std::mutex> lock(service().mutex);
        for (const ModuleTransition& t : batch) {
            if (t.newState != module_state::kUnloaded && t.newState != module_state::kAbsent
                && t.newState != module_state::kError)
                continue;
            const auto it = service().watches.find(t.module);
            if (it == service().watches.end()) continue;
            for (auto& [name, watch] : it->second) ended.push_back(watch);
            service().watches.erase(it);
        }
    }
    release(ended);
    for (const ModuleTransition& t : batch)
        emit("moduleStateChanged",
             json::array({t.module, t.oldState, t.newState,
                          t.reason ? json(*t.reason) : json(nullptr), t.seq}));
}

} // namespace

void setTransports(const std::string& text)
{
    std::lock_guard<std::mutex> lock(config().mutex);
    config().transports = text;
}

void setShutdownHandler(ShutdownHandler handler, void* userData)
{
    std::lock_guard<std::mutex> lock(config().mutex);
    config().shutdown = handler;
    config().shutdownData = userData;
}

void setOperatorResolver(OperatorResolver resolver, void* userData)
{
    std::lock_guard<std::mutex> lock(config().mutex);
    config().operators = resolver;
    config().operatorData = userData;
}

void setExtension(Extension extension, const std::string& methodsJson, void* userData)
{
    const json listed = json::parse(methodsJson.empty() ? "[]" : methodsJson, nullptr, false);
    std::lock_guard<std::mutex> lock(config().mutex);
    config().extension = extension;
    config().extensionMethods = listed.is_array() ? listed : json::array();
    config().extensionData = userData;
}

void setShellIdentity(const std::string& name)
{
    std::lock_guard<std::mutex> lock(config().mutex);
    config().shell = name;
}

std::string shellIdentity()
{
    std::lock_guard<std::mutex> lock(config().mutex);
    return config().shell;
}

Hooks hooks()
{
    std::lock_guard<std::mutex> lock(config().mutex);
    Hooks value;
    value.shutdown = config().shutdown;
    value.shutdownData = config().shutdownData;
    value.operators = config().operators;
    value.operatorData = config().operatorData;
    value.extension = config().extension;
    value.extensionMethods = config().extensionMethods.dump();
    value.extensionData = config().extensionData;
    return value;
}

void resetConfiguration()
{
    std::lock_guard<std::mutex> lock(config().mutex);
    config().transports.clear();
    config().shutdown = nullptr;
    config().shutdownData = nullptr;
    config().operators = nullptr;
    config().operatorData = nullptr;
    config().extension = nullptr;
    config().extensionMethods = json::array();
    config().extensionData = nullptr;
    config().shell.clear();
}

bool start()
{
    Service& s = service();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.provider) return true;
    }
    if (!authority::attached()) {
        spdlog::error("core_service: no token authority is running, so it cannot be admitted");
        return false;
    }
    const std::string credential = authority::admit(kName, "module");
    if (credential.empty()) {
        spdlog::error("core_service: capability_module refused to admit it");
        return false;
    }
    json transports = json::array({{{"protocol", "inproc"}}, {{"protocol", "qt_remote_plain"}}});
    {
        std::lock_guard<std::mutex> lock(config().mutex);
        const json extra = json::parse(config().transports.empty() ? "[]" : config().transports,
                                       nullptr, false);
        if (extra.is_array())
            for (const auto& t : extra) transports.push_back(t);
    }
    lp_provider* provider = lp_provider_create(kName, transports.dump().c_str());
    if (!provider) {
        spdlog::error("core_service: its provider could not be created");
        authority::retire(kName);
        return false;
    }
    // Its own calls (an operator's package commands) go as itself.
    if (lp_token_isolate_identity(kName) != LP_OK
        || lp_token_adopt_credential(kName, credential.c_str()) != LP_OK
        || lp_token_save_for(kName, "capability_module", credential.c_str()) != LP_OK)
        spdlog::warn("core_service: no identity of its own for its calls");
    lp_provider_set_max_concurrent_calls(provider, kConcurrentCalls);
    lp_provider_save_token(provider, "core", credential.c_str());
    lp_provider_set_caller_resolver(provider, &resolveCaller, nullptr);
    if (lp_provider_register(provider, &dispatch, &methods, nullptr, nullptr) != LP_OK) {
        spdlog::error("core_service: its provider could not be published");
        lp_provider_destroy(provider);
        authority::retire(kName);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.provider = provider;
    }
    ModuleManager::registry().registerEmbedded(kName);
    s.sink = ModuleStateObserver::instance().addSink(&publishTransitions);
    spdlog::info("core_service is published");
    return true;
}

void stop()
{
    Service& s = service();
    lp_provider* provider = nullptr;
    std::vector<Watch> watches;
    std::map<std::pair<std::string, std::string>, lp_client*> clients;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        provider = s.provider;
        s.provider = nullptr;
        for (auto& [module, events] : s.watches)
            for (auto& [name, watch] : events) watches.push_back(watch);
        s.watches.clear();
        clients.swap(s.clients);
        s.consumers.clear();
    }
    if (s.sink) {
        ModuleStateObserver::instance().removeSink(s.sink);
        s.sink = 0;
    }
    release(watches);
    for (auto& [key, client] : clients)
        if (client) lp_client_destroy(client);
    if (!provider) return;
    lp_provider_destroy(provider);
    ModuleManager::registry().forgetEmbedded(kName);
    authority::retire(kName);
}

} // namespace logos::core_service
