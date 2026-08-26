#include "module_state_observer.h"

#include <utility>

namespace logos {

ModuleStateObserver& ModuleStateObserver::instance()
{
    // Function-local static: one observer per liblogos IMAGE.
    //
    // On ELF that is one per process, because the dynamic linker interposes a
    // single definition. On PE it is one per image — a host that links
    // liblogos_core into two binaries gets two observers, which is the same
    // trap that gave Basecamp three TokenManagers. That is acceptable here and
    // is not acceptable for TokenManager, for one reason: this object holds no
    // cross-image contract. A second observer with no sink installed simply
    // does nothing, whereas a second TokenManager silently loses tokens.
    static ModuleStateObserver s_instance;
    return s_instance;
}

void ModuleStateObserver::setSink(Sink sink)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sink = std::move(sink);
}

bool ModuleStateObserver::hasSink() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<bool>(m_sink);
}

uint64_t ModuleStateObserver::nextSeq()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return ++m_seq;
}

void ModuleStateObserver::record(const std::string& module,
                                 const std::string& oldState,
                                 const std::string& newState,
                                 std::optional<std::string> instance,
                                 std::optional<int64_t> pid,
                                 std::optional<std::string> reason)
{
    // A no-op is not a transition. Dropped here rather than at the sink so the
    // guarantee holds for every consumer: modules_state refuses old == new, and
    // a feed that emitted them would be generating traffic that is defined to
    // be discarded.
    if (module.empty() || oldState.empty() || newState.empty())
        return;
    if (oldState == newState)
        return;

    ModuleTransition t;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // No sink means nobody is listening, so buffering would be an unbounded
        // leak in the normal case — a host with no modules_state loaded runs
        // for weeks. Checked under the lock so it cannot race setSink().
        if (!m_sink)
            return;

        t.seq = ++m_seq;
        t.module = module;
        t.oldState = oldState;
        t.newState = newState;
        t.instance = std::move(instance);
        t.pid = std::move(pid);
        t.reason = std::move(reason);
        m_pending.push_back(std::move(t));
    }
}

void ModuleStateObserver::flush()
{
    // Compute under the lock, dispatch after releasing it — the discipline the
    // header's rule 1 exists to enforce. The sink is copied out too: calling it
    // while holding m_mutex would deadlock the moment a sink re-entered
    // record(), and a sink that reacts to a transition by causing another one
    // is an ordinary thing to write.
    std::vector<ModuleTransition> batch;
    Sink sink;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pending.empty() || !m_sink)
            return;
        batch.swap(m_pending);
        sink = m_sink;
    }
    sink(batch);
}

void ModuleStateObserver::clearPending()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending.clear();
}

}  // namespace logos
