#include "peering_link.h"

#include "core_service/embedded_core_service.h"
#include "module_manager.h"
#include "module_registry.h"
#include "module_state_observer.h"

#include "logos_protocol.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace logos::peering_link {
namespace {

using json = nlohmann::json;

constexpr const char* kPeering = "peering_module";

struct State {
    std::mutex mutex;
    json config;                 // null when not configured
    bool started = false;
    std::set<std::string> exports;
    std::map<std::string, std::int64_t> epochs;   // the load each announcement is for
    std::map<std::string, std::string> roles;     // announced: facade | export
    std::map<std::string, std::pair<std::string, std::string>> importStates;
    std::map<std::string, std::string> reflected; // a loaded facade's shown state
    std::shared_ptr<lp_client> client;
    std::vector<lp_subscription*> subscriptions;

    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    std::thread worker;
    bool stopping = false;
};

State& state()
{
    static State s;
    return s;
}

json call(const std::string& method, const json& args)
{
    return ModuleManager::callAsRuntime(kPeering, method, args);
}

bool failed(const json& reply) { return !reply.is_object() || reply.contains("error"); }

// On the link's own thread: event callbacks and exits must not block.
void post(std::function<void()> job)
{
    State& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.stopping || !s.worker.joinable()) return;
        s.jobs.push_back(std::move(job));
    }
    s.wake.notify_all();
}

void run()
{
    State& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    for (;;) {
        s.wake.wait(lock, [&] { return s.stopping || !s.jobs.empty(); });
        if (s.stopping) return;
        auto job = std::move(s.jobs.front());
        s.jobs.pop_front();
        lock.unlock();
        job();
        lock.lock();
    }
}

// What the registry knows a facade by: the import's name, a universal plain
// module whose calls wait upstream.
json facadeMetadata(const std::string& name, const json& rule)
{
    return {{"name", name},
            {"version", rule.value("version", std::string("0.0.0"))},
            {"type", "core"},
            {"interface", "universal"},
            {"transport", "qt_remote_plain"},
            {"concurrency", "multi"},
            {"dependencies", json::array()},
            {"logos_protocol_version", LOGOS_PROTOCOL_VERSION_STRING},
            {"description", "Imported: " + rule.value("module", name) + " on "
                                + rule.value("peer_alias", rule.value("from", std::string()))},
            {"peer_facade", {{"from", rule.value("from", "")}, {"module", rule.value("module", "")}}}};
}

void refreshExports()
{
    const json reply = call("exports", json::array());
    if (failed(reply)) return;
    std::set<std::string> names;
    for (const auto& item : reply.items()) names.insert(item.key());
    std::lock_guard<std::mutex> lock(state().mutex);
    state().exports = std::move(names);
}

void refreshImports()
{
    const json imports = call("imports", json::array());
    if (failed(imports)) return;
    auto& registry = ModuleManager::registry();
    for (const auto& item : imports.items()) {
        const std::string& name = item.key();
        const json& rule = item.value();
        if (registry.isFacade(name) && registry.isLoaded(name)) continue;
        const bool preferRemote = rule.value("prefer", std::string("remote")) == "remote";
        if (!registry.isFacade(name)
            && !registry.registerFacade(name, facadeMetadata(name, rule), preferRemote)) {
            spdlog::info("Import {} is served by the local module of that name (prefer {})", name,
                         preferRemote ? "remote, but it is loaded" : "local");
            continue;
        }
        if (!ModuleManager::loadModule(name.c_str()))
            spdlog::warn("Import {}: its facade did not load", name);
    }
    for (const std::string& name : registry.facadeNames()) {
        if (imports.contains(name)) continue;
        if (registry.isLoaded(name)) ModuleManager::unloadModule(name.c_str());
        registry.forgetFacade(name);
    }
}

// The facade's state as its consumers see it: ready while its import is.
void reflect(const std::string& name)
{
    auto& registry = ModuleManager::registry();
    std::string shown;
    std::string next;
    std::string reason;
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        const auto it = s.importStates.find(name);
        if (it == s.importStates.end() || !registry.isFacade(name) || !registry.isLoaded(name)) return;
        const std::string& imported = it->second.first;
        if (imported != "ready" && imported != "error") return;
        next = imported == "ready" ? module_state::kReady : module_state::kError;
        shown = s.reflected.count(name) ? s.reflected[name] : std::string(module_state::kLoaded);
        if (shown == next) return;
        s.reflected[name] = next;
        reason = it->second.second;
    }
    auto& observer = ModuleStateObserver::instance();
    if (next == module_state::kReady) {
        const uint64_t epoch = registry.loadEpoch(name);
        registry.beginPublishWatch(name);
        registry.markPublished(name, epoch);
        observer.record(name, shown, next);
    } else {
        observer.record(name, shown, next, std::nullopt, std::nullopt, reason);
    }
    observer.flush();
}

void onEvent(const char* event, const char* data, void*)
{
    const std::string name = event ? event : "";
    const json args = json::parse(data ? data : "[]", nullptr, false);
    if (name == "importsChanged") {
        post(refreshImports);
    } else if (name == "exportsChanged") {
        post(refreshExports);
    } else if (name == "importStateChanged" && args.is_array() && args.size() >= 3
               && args[0].is_string() && args[1].is_string()) {
        const std::string facade = args[0].get<std::string>();
        {
            std::lock_guard<std::mutex> lock(state().mutex);
            state().importStates[facade] = {args[1].get<std::string>(),
                                            args[2].is_string() ? args[2].get<std::string>() : ""};
        }
        post([facade] { reflect(facade); });
    }
}

void announceExit(const std::string& name, const std::string& role, std::int64_t epoch)
{
    const json reply = call(role == "facade" ? "facadeExited" : "exportExited",
                            json::array({name, epoch}));
    if (failed(reply)) spdlog::debug("peering_module did not take the exit of {}", name);
}

} // namespace

bool setConfig(const std::string& text, std::string& error)
{
    json config = text.empty() ? json() : json::parse(text, nullptr, false);
    if (!text.empty() && !config.is_object()) {
        error = "the peering configuration is not a JSON object";
        return false;
    }
    std::lock_guard<std::mutex> lock(state().mutex);
    state().config = std::move(config);
    return true;
}

bool configured()
{
    std::lock_guard<std::mutex> lock(state().mutex);
    return state().config.is_object();
}

std::string withExportListener(const std::string& transportSet, const std::string& host)
{
    json set = transportSet.empty() ? json::array() : json::parse(transportSet, nullptr, false);
    if (!set.is_array()) set = json::array();
    if (set.empty()) set.push_back({{"protocol", "qt_remote_plain"}});
    set.push_back({{"protocol", "tls_tcp"}, {"host", host}, {"port", 0}});
    return set.dump();
}

void start()
{
    State& s = state();
    json config;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.config.is_object() || s.started) return;
        config = s.config;
    }
    if (!ModuleManager::registry().isKnown(kPeering)) {
        spdlog::warn("Peering is configured, but peering_module is not among the bundled modules");
        return;
    }
    if (!ModuleManager::loadModuleWithDependencies(kPeering)) {
        spdlog::error("peering_module did not load; this runtime links with no other");
        return;
    }
    // The shell this runtime serves manages peering, whatever the document says.
    config["shell"] = logos::core_service::shellIdentity();
    const json configured = call("configure", json::array({config}));
    if (failed(configured)) {
        spdlog::error("peering_module refused its configuration: {}",
                      configured.is_object() ? configured.value("error", std::string()) : "no answer");
        return;
    }
    spdlog::info("Peering: runtime {}", configured.value("runtime_id", std::string()));
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.started = true;
        s.stopping = false;
        s.client = ModuleManager::runtimeClient(kPeering);
        for (const char* event : {"importsChanged", "exportsChanged", "importStateChanged"})
            if (s.client)
                if (lp_subscription* sub = lp_subscribe(s.client.get(), event, &onEvent, nullptr))
                    s.subscriptions.push_back(sub);
        s.worker = std::thread(run);
    }
    refreshExports();
    // Facades load in the background: the runtime's ready line never waits on a peer.
    post(refreshImports);
}

void stop()
{
    State& s = state();
    std::thread worker;
    std::vector<lp_subscription*> subscriptions;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.stopping = true;
        s.started = false;
        s.jobs.clear();
        worker = std::move(s.worker);
        subscriptions.swap(s.subscriptions);
    }
    s.wake.notify_all();
    if (worker.joinable()) worker.join();
    for (lp_subscription* sub : subscriptions) lp_unsubscribe(sub);
    std::lock_guard<std::mutex> lock(s.mutex);
    s.client.reset();
    s.exports.clear();
    s.epochs.clear();
    s.roles.clear();
    s.importStates.clear();
    s.reflected.clear();
    s.config = json();
}

Announcement::Announcement(std::string name, std::string role, std::int64_t epoch)
    : m_name(std::move(name)), m_role(std::move(role)), m_epoch(epoch)
{
}

Announcement::Announcement(Announcement&& other) noexcept
    : m_name(std::move(other.m_name)), m_role(std::move(other.m_role)), m_epoch(other.m_epoch),
      m_committed(other.m_committed)
{
    other.m_role.clear();
}

Announcement::~Announcement()
{
    if (m_role.empty() || m_committed) return;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().roles.erase(m_name);
    }
    const std::string name = m_name;
    const std::string role = m_role;
    const std::int64_t epoch = m_epoch;
    post([name, role, epoch] { announceExit(name, role, epoch); });
}

void Announcement::commit() { m_committed = true; }

Announcement beforeSpawn(const std::string& name, const std::string& format, bool inProcess,
                         std::string& transportSet)
{
    State& s = state();
    std::string role;
    std::string host = "0.0.0.0";
    std::int64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.started || name == kPeering) return {};
        if (ModuleManager::registry().isFacade(name)) role = "facade";
        else if (format == "native-cdylib" && s.exports.count(name)) role = "export";
        if (role.empty()) return {};
        if (role == "export" && inProcess) {
            spdlog::warn("Module {} is exported but runs in-process; only a host process "
                         "exports, so it loads unexported", name);
            return {};
        }
        const json control = s.config.value("control", json::object());
        if (control.is_object()) host = control.value("host", host);
        epoch = ++s.epochs[name];
        s.roles[name] = role;
    }
    const json reply = call(role == "facade" ? "facadeLoaded" : "exportLoaded",
                            json::array({name, epoch}));
    if (failed(reply)) {
        spdlog::warn("peering_module did not take the {} of {}: {}", role, name,
                     reply.is_object() ? reply.value("error", std::string()) : "no answer");
        std::lock_guard<std::mutex> lock(s.mutex);
        s.roles.erase(name);
        return {};
    }
    if (role == "export") transportSet = withExportListener(transportSet, host);
    return Announcement(name, role, epoch);
}

void exited(const std::string& name)
{
    std::string role;
    std::int64_t epoch = 0;
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        const auto it = s.roles.find(name);
        if (it == s.roles.end()) return;
        role = it->second;
        epoch = s.epochs[name];
        s.roles.erase(it);
        s.reflected.erase(name);
    }
    post([name, role, epoch] { announceExit(name, role, epoch); });
}

void facadeLoaded(const std::string& name)
{
    post([name] { reflect(name); });
}

} // namespace logos::peering_link
