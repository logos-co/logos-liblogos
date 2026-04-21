#include "runtime_registry.h"

namespace LogosCore {

void RuntimeRegistry::registerRuntime(std::shared_ptr<ModuleRuntime> runtime)
{
    std::lock_guard lock(m_mutex);
    m_runtimes.push_back(std::move(runtime));
}

std::shared_ptr<ModuleRuntime> RuntimeRegistry::select(const ModuleDescriptor& desc) const
{
    std::lock_guard lock(m_mutex);

    // Explicit id override: caller pinned a specific runtime id.
    if (desc.runtimeConfig.contains("id")) {
        std::string requestedId = desc.runtimeConfig.at("id").get<std::string>();
        for (const auto& rt : m_runtimes) {
            if (rt->id() == requestedId)
                return rt;
        }
        // Unknown explicit id — don't fall through to canHandle.
        return nullptr;
    }

    // Format/capability-based: first runtime that accepts this descriptor.
    for (const auto& rt : m_runtimes) {
        if (rt->canHandle(desc))
            return rt;
    }
    return nullptr;
}

void RuntimeRegistry::terminateAll()
{
    std::lock_guard lock(m_mutex);
    for (const auto& rt : m_runtimes)
        rt->terminateAll();
}

std::unordered_map<std::string, int64_t> RuntimeRegistry::getAllPids() const
{
    std::lock_guard lock(m_mutex);
    std::unordered_map<std::string, int64_t> result;
    for (const auto& rt : m_runtimes) {
        auto pids = rt->getAllPids();
        result.insert(pids.begin(), pids.end());
    }
    return result;
}

void RuntimeRegistry::clearForTests()
{
    std::lock_guard lock(m_mutex);
    m_runtimes.clear();
}

} // namespace LogosCore
