#include "inproc_module_loader.h"
#include "bootstrap_policy.h"
#include "module_manager.h"
#include "module_registry.h"

#include <native_module_host.h>
#include <logos_protocol.h>
#include <logos_runtime_delegate.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <unordered_set>
#include <vector>

namespace LogosCore {
namespace {

using json = nlohmann::json;

constexpr auto kCredentialWait = std::chrono::seconds(10);
// Never shortened by a silent subprocess host: this loader always answers.
constexpr std::chrono::milliseconds kInitDeadline{10000};
constexpr auto kStopDeadline = std::chrono::seconds(3);

bool placementValue(const json& value, bool& inProcess)
{
    if (!value.is_string()) return false;
    const std::string text = value.get<std::string>();
    if (text != "inproc" && text != "subprocess") return false;
    inProcess = text == "inproc";
    return true;
}

// The provider serves inproc, plus what it was configured with (the local socket
// when nothing), so processes outside this one reach it as before.
std::string transportSetFor(const std::string& configured)
{
    json set = configured.empty() ? json::array() : json::parse(configured, nullptr, false);
    if (set.is_object()) set = json::array({set});
    if (!set.is_array()) set = json::array();
    if (set.empty()) set.push_back({{"protocol", "qt_remote_plain"}});
    const bool served = std::any_of(set.begin(), set.end(), [](const json& entry) {
        return entry.is_object() && entry.value("protocol", std::string{}) == "inproc";
    });
    if (!served) set.insert(set.begin(), json{{"protocol", "inproc"}});
    return set.dump();
}

unsigned maxCallsFor(const json& metadata)
{
    const std::string concurrency = metadata.is_object()
        ? metadata.value("concurrency", std::string{"single"}) : std::string{"single"};
    int workers = 0;
    if (metadata.is_object())
        if (auto it = metadata.find("max_workers"); it != metadata.end() && it->is_number_integer())
            workers = it->get<int>();
    return logos::native_host::maxCallsFor(concurrency, workers);
}

// Images opened once stay mapped for the process's lifetime, whichever loader
// opened them: running one again needs a restart (module ABI v2 lifts this).
bool claimImage(const std::string& path)
{
    static std::mutex mutex;
    static std::unordered_set<std::string> mapped;
    std::lock_guard<std::mutex> lock(mutex);
    return mapped.insert(path).second;
}

PlacementDecision decisionFor(const ModuleDescriptor& desc)
{
    return decidePlacement(desc.name, desc.rawMetadata, desc.format,
                           ModuleManager::registry().isBundled(desc.name),
                           ModuleManager::placementPolicy());
}

} // namespace

struct InprocModuleLoader::Entry {
    enum class State { Starting, Loaded, Failed };

    std::string name;
    std::mutex mutex;
    std::condition_variable changed;
    std::string credential;
    bool credentialArrived = false;
    State state = State::Starting;
    std::string reason;
    std::atomic<bool> abandoned{false};
    logos::native_host::Module module;
    const lp_runtime_delegate_v1* delegate = nullptr;
    std::thread thread;

    void finish(State outcome, std::string why = {})
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            state = outcome;
            reason = std::move(why);
        }
        changed.notify_all();
    }

    bool waitSettled(std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, limit, [&] { return state != State::Starting; });
    }
};

namespace {

// Kept alive for good: something in the module may still run on it.
void leak(std::shared_ptr<void> entry)
{
    (void)new std::shared_ptr<void>(std::move(entry));
}

} // namespace

bool parsePlacementPolicy(const std::string& text, PlacementPolicy& out, std::string& error)
{
    PlacementPolicy policy;
    const json doc = json::parse(text, nullptr, false);
    if (!doc.is_object()) {
        error = "the placement policy is not a JSON object";
        return false;
    }
    if (auto it = doc.find("default"); it != doc.end()
        && !placementValue(*it, policy.defaultInProcess)) {
        error = "\"default\" must be \"subprocess\" or \"inproc\"";
        return false;
    }
    if (auto it = doc.find("modules"); it != doc.end()) {
        if (!it->is_object()) {
            error = "\"modules\" must be an object";
            return false;
        }
        for (const auto& [name, value] : it->items()) {
            bool inProcess = false;
            if (!placementValue(value, inProcess)) {
                error = "the placement of '" + name + "' must be \"subprocess\" or \"inproc\"";
                return false;
            }
            policy.modules[name] = inProcess;
        }
    }
    if (auto it = doc.find("single_process"); it != doc.end()) {
        if (!it->is_boolean()) {
            error = "\"single_process\" must be a boolean";
            return false;
        }
        policy.singleProcess = it->get<bool>();
    }
    out = std::move(policy);
    return true;
}

PlacementDecision decidePlacement(const std::string& name, const json& sidecar,
                                  const std::string& format, bool bundled,
                                  const PlacementPolicy& policy)
{
    using logos::bootstrap::Placement;
    const logos::bootstrap::Row* row = logos::bootstrap::rowFor(name);
    bool wanted = policy.defaultInProcess;
    if (row && row->pinnedInProcess) wanted = true;
    else if (auto it = policy.modules.find(name); it != policy.modules.end()) wanted = it->second;
    else if (policy.singleProcess) wanted = true;
    else if (row && row->placement != Placement::Default)
        wanted = row->placement == Placement::InProcess;
    if (!wanted) return {};

    std::string why;
    if (format != "native-cdylib") why = "it is not a native module";
    else if (!bundled) why = "it is not bundled";
    else if (!sidecar.is_object() || !sidecar.value("inproc_eligible", false))
        why = sidecar.is_object() && sidecar.contains("inproc_ineligible_reason")
            ? sidecar.value("inproc_ineligible_reason", std::string{})
            : std::string("its build did not stamp it in-process eligible");
    if (why.empty()) return {true, false, {}};
    if (policy.singleProcess)
        return {false, true, "single_process, and " + name + " cannot run in-process: " + why};
    return {false, false, why};
}

InprocModuleLoader::InprocModuleLoader() = default;

// At exit nothing is stopped: that is ModuleManager's teardown, in order.
InprocModuleLoader::~InprocModuleLoader()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [name, entry] : m_entries) {
        if (entry->thread.joinable()) entry->thread.detach();
        leak(entry);
    }
}

bool InprocModuleLoader::canHandle(const ModuleDescriptor& desc) const
{
    const PlacementDecision decision = decisionFor(desc);
    if (!decision.inProcess && !decision.refused && !decision.reason.empty())
        spdlog::info("{} runs in a subprocess: {}", desc.name, decision.reason);
    return decision.inProcess || decision.refused;
}

bool InprocModuleLoader::load(const ModuleDescriptor& desc,
                              std::function<void(const std::string&)> /*onTerminated*/,
                              LoadedModuleHandle& out)
{
    const PlacementDecision decision = decisionFor(desc);
    if (!decision.inProcess) {
        spdlog::error("Refusing to load {}: {}", desc.name,
                      decision.reason.empty() ? "it is not placed in-process" : decision.reason);
        return false;
    }

    auto entry = std::make_shared<Entry>();
    entry->name = desc.name;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_entries.count(desc.name)) return false;
        if (!claimImage(desc.path)) {
            spdlog::error("{} already ran in this process; loading it again needs a restart",
                          desc.name);
            return false;
        }
        m_entries[desc.name] = entry;
    }

    logos::native_host::Options options;
    options.name = desc.name;
    options.path = desc.path;
    options.transportSet = transportSetFor(desc.transportSetJson);
    options.maxCalls = maxCallsFor(desc.rawMetadata);
    options.instancePersistencePath = desc.instancePersistencePath;
    const std::vector<std::string> grants = logos::bootstrap::hostServicesFor(desc.name, true);
    options.hostServices = grants.empty() ? std::string{} : json(grants).dump();

    // The bring-up waits for the credential sendToken() hands over, as a
    // subprocess host waits on its stdin.
    entry->thread = std::thread([entry, options]() mutable {
        std::string credential;
        {
            std::unique_lock<std::mutex> lock(entry->mutex);
            if (!entry->changed.wait_for(lock, kCredentialWait, [&] {
                    return entry->credentialArrived || entry->abandoned.load();
                }) || !entry->credentialArrived) {
                lock.unlock();
                return entry->finish(Entry::State::Failed, "no credential arrived");
            }
            credential = entry->credential;
        }
        const std::string& name = entry->name;
        // Its own store, so it never runs on the runtime's.
        if (lp_token_isolate_identity(name.c_str()) != LP_OK
            || lp_token_adopt_credential(name.c_str(), credential.c_str()) != LP_OK
            || lp_token_save_for(name.c_str(), "capability_module", credential.c_str()) != LP_OK
            || lp_token_save_for(name.c_str(), "core", credential.c_str()) != LP_OK)
            return entry->finish(Entry::State::Failed, "could not give it an identity of its own");
        const std::string grantsJson = options.hostServices.empty() ? "[]" : options.hostServices;
        entry->delegate = lp_runtime_delegate_create(name.c_str(), grantsJson.c_str());
        if (!entry->delegate)
            return entry->finish(Entry::State::Failed, "the runtime refused it a delegate");

        options.credential = credential;
        options.delegate = entry->delegate;
        options.stillWanted = [raw = entry.get()] { return !raw->abandoned.load(); };
        std::string error;
        if (!entry->module.start(options, error)) {
            lp_runtime_delegate_release(entry->delegate);
            return entry->finish(Entry::State::Failed, error);
        }
        entry->finish(Entry::State::Loaded);
    });

    out.name = desc.name;
    out.pid = -1;
    out.endpoint = "inproc://" + desc.name;
    return true;
}

bool InprocModuleLoader::sendToken(const std::string& name, const std::string& token)
{
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_entries.find(name);
        if (it == m_entries.end()) return false;
        entry = it->second;
    }
    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        entry->credential = token;
        entry->credentialArrived = true;
    }
    entry->changed.notify_all();
    return true;
}

LoadOutcome InprocModuleLoader::awaitLoad(const std::string& name,
                                          std::chrono::milliseconds timeout)
{
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_entries.find(name);
        if (it == m_entries.end()) return {LoadVerdict::Failed, "the module is not loading"};
        entry = it->second;
    }
    if (!entry->waitSettled(std::max(timeout, kInitDeadline))) {
        entry->abandoned = true;
        entry->changed.notify_all();
        return {LoadVerdict::Failed, "in-process initialization did not finish in time"};
    }
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (entry->state == Entry::State::Loaded) return {LoadVerdict::Loaded, {}};
    return {LoadVerdict::Failed, entry->reason};
}

std::shared_ptr<InprocModuleLoader::Entry> InprocModuleLoader::take(const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(name);
    if (it == m_entries.end()) return nullptr;
    auto entry = it->second;
    m_entries.erase(it);
    return entry;
}

void InprocModuleLoader::terminate(const std::string& name)
{
    std::shared_ptr<Entry> entry = take(name);
    if (!entry) return;
    entry->abandoned = true;
    entry->changed.notify_all();
    if (!entry->waitSettled(std::chrono::duration_cast<std::chrono::milliseconds>(kStopDeadline))) {
        spdlog::error("{}: in-process initialization is stuck; leaving it running", name);
        entry->thread.detach();
        return leak(entry);
    }
    entry->thread.join();
    if (entry->state == Entry::State::Loaded
        && !entry->module.stop(std::chrono::steady_clock::now() + kStopDeadline,
                               logos::native_host::Teardown::InProcess)) {
        spdlog::error("{}: a call or its unload outlived the deadline; leaving it running", name);
        return leak(entry);
    }
    if (entry->state == Entry::State::Loaded && entry->delegate)
        lp_runtime_delegate_release(entry->delegate);
}

void InprocModuleLoader::terminateAll()
{
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [name, entry] : m_entries) names.push_back(name);
    }
    // User modules first, capability_module last: the others may still call it.
    std::stable_sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        return logos::bootstrap::teardownRank(a) < logos::bootstrap::teardownRank(b);
    });
    for (const std::string& name : names) terminate(name);
}

bool InprocModuleLoader::hasModule(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.count(name) > 0;
}

void* InprocModuleLoader::symbolOf(const std::string& name, const char* symbol) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(name);
    return it == m_entries.end() ? nullptr : it->second->module.symbol(symbol);
}

lp_provider* InprocModuleLoader::providerOf(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(name);
    return it == m_entries.end() ? nullptr : it->second->module.provider();
}

} // namespace LogosCore
