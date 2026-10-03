#include "inproc_facade_loader.h"
#include "inproc_module_loader.h"
#include "module_manager.h"
#include "module_registry.h"

#include <logos/peering/facade.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <future>
#include <thread>
#include <vector>

namespace LogosCore {
namespace {

using json = nlohmann::json;

constexpr auto kCredentialWait = std::chrono::seconds(10);
constexpr std::chrono::milliseconds kInitDeadline{10000};
constexpr std::chrono::milliseconds kStopDeadline{5000};

// As logos_host_remote: a `multi` import serves max_workers calls at once, or 16.
unsigned maxCallsFor(const json& metadata)
{
    if (!metadata.is_object() || metadata.value("concurrency", std::string("single")) != "multi")
        return 1;
    const auto it = metadata.find("max_workers");
    return it != metadata.end() && it->is_number_integer() && it->get<int>() > 0
        ? static_cast<unsigned>(it->get<int>()) : 16u;
}

PlacementDecision decisionFor(const ModuleDescriptor& desc)
{
    return decidePlacement(desc.name, desc.rawMetadata, desc.format,
                           ModuleManager::registry().isBundled(desc.name),
                           ModuleManager::placementPolicy());
}

} // namespace

struct InprocFacadeLoader::Entry {
    enum class State { Starting, Running, Failed };

    std::string name;
    std::mutex mutex;
    std::condition_variable changed;
    std::string credential;
    bool credentialArrived = false;
    State state = State::Starting;
    std::string reason;
    std::atomic<bool> abandoned{false};
    std::unique_ptr<logos::peering::Facade> facade;
    std::thread thread;

    bool waitSettled(std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, limit, [&] { return state != State::Starting; });
    }
};

namespace {

// Kept alive for good: its facade may still be stopping on another thread.
void leak(std::shared_ptr<void> entry)
{
    (void)new std::shared_ptr<void>(std::move(entry));
}

} // namespace

InprocFacadeLoader::InprocFacadeLoader() = default;

// At exit nothing is stopped: that is ModuleManager's teardown.
InprocFacadeLoader::~InprocFacadeLoader()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [name, entry] : m_entries) {
        if (entry->thread.joinable()) entry->thread.detach();
        leak(entry);
    }
}

bool InprocFacadeLoader::canHandle(const ModuleDescriptor& desc) const
{
    return desc.format == kFacadeFormat && decisionFor(desc).inProcess;
}

bool InprocFacadeLoader::load(const ModuleDescriptor& desc,
                              std::function<void(const std::string&)> /*onTerminated*/,
                              LoadedModuleHandle& out)
{
    if (!canHandle(desc)) {
        spdlog::error("Refusing to run the facade {} in-process: the runtime is not single-process",
                      desc.name);
        return false;
    }
    auto entry = std::make_shared<Entry>();
    entry->name = desc.name;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_entries.count(desc.name)) return false;
        m_entries[desc.name] = entry;
    }

    logos::peering::FacadeOptions options;
    options.name = desc.name;
    options.transportSet = inprocTransportSet(desc.transportSetJson);
    options.maxCalls = maxCallsFor(desc.rawMetadata);

    // Waits for the credential sendToken() hands over, as logos_host_remote on its stdin.
    entry->thread = std::thread([entry, options]() mutable {
        {
            std::unique_lock<std::mutex> lock(entry->mutex);
            if (!entry->changed.wait_for(lock, kCredentialWait, [&] {
                    return entry->credentialArrived || entry->abandoned.load();
                }) || !entry->credentialArrived) {
                entry->state = Entry::State::Failed;
                entry->reason = "no credential arrived";
                lock.unlock();
                entry->changed.notify_all();
                return;
            }
            options.credential = entry->credential;
        }
        auto facade = std::make_unique<logos::peering::Facade>(options);
        std::string error;
        const bool started = facade->start(error);
        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            if (started && !entry->abandoned) {
                entry->facade = std::move(facade);
                entry->state = Entry::State::Running;
            } else {
                entry->state = Entry::State::Failed;
                entry->reason = started ? "abandoned while it started" : error;
            }
        }
        entry->changed.notify_all();
        // A facade not kept stops here, on this thread.
    });

    out.name = desc.name;
    out.pid = -1;
    out.endpoint = "inproc://" + desc.name;
    return true;
}

bool InprocFacadeLoader::sendToken(const std::string& name, const std::string& token)
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

LoadOutcome InprocFacadeLoader::awaitLoad(const std::string& name, std::chrono::milliseconds timeout)
{
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_entries.find(name);
        if (it == m_entries.end()) return {LoadVerdict::Failed, "the facade is not loading"};
        entry = it->second;
    }
    if (!entry->waitSettled(std::max(timeout, kInitDeadline))) {
        entry->abandoned = true;
        entry->changed.notify_all();
        return {LoadVerdict::Failed, "the facade did not start in time"};
    }
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (entry->state == Entry::State::Running) return {LoadVerdict::Loaded, {}};
    return {LoadVerdict::Failed, entry->reason};
}

std::shared_ptr<InprocFacadeLoader::Entry> InprocFacadeLoader::take(const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(name);
    if (it == m_entries.end()) return nullptr;
    auto entry = it->second;
    m_entries.erase(it);
    return entry;
}

void InprocFacadeLoader::terminate(const std::string& name)
{
    std::shared_ptr<Entry> entry = take(name);
    if (!entry) return;
    entry->abandoned = true;
    entry->changed.notify_all();
    if (!entry->waitSettled(kStopDeadline)) {
        // Its thread stops the facade once start() returns.
        spdlog::warn("{}: the facade is still starting; it stops when it has", name);
        entry->thread.detach();
        return leak(entry);
    }
    entry->thread.join();
    if (!entry->facade) return;
    // stop() waits for calls in flight upstream; past the deadline it finishes alone.
    auto stopped = std::make_shared<std::promise<void>>();
    std::future<void> done = stopped->get_future();
    std::thread([entry, stopped] {
        entry->facade->stop();
        stopped->set_value();
    }).detach();
    if (done.wait_for(kStopDeadline) != std::future_status::ready)
        spdlog::warn("{}: calls in flight outlived the deadline; the facade stops when they end", name);
}

void InprocFacadeLoader::terminateAll()
{
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [name, entry] : m_entries) names.push_back(name);
    }
    for (const std::string& name : names) terminate(name);
}

bool InprocFacadeLoader::hasModule(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.count(name) > 0;
}

} // namespace LogosCore
