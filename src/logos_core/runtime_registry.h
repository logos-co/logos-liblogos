#ifndef RUNTIME_REGISTRY_H
#define RUNTIME_REGISTRY_H

#include "module_runtime.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace LogosCore {

// Holds all registered ModuleRuntime instances and selects one for a given descriptor.
// Thread-safe (all operations protected by an internal mutex).
class RuntimeRegistry {
public:
    // Register a runtime. Runtimes are consulted in registration order when
    // no explicit runtimeConfig["id"] is present.
    void registerRuntime(std::shared_ptr<ModuleRuntime> runtime);

    // Select a runtime for the descriptor.
    // Selection order:
    //   1. If desc.runtimeConfig["id"] is set, return the matching runtime by id.
    //      Returns nullptr if the id is unknown (does not fall through to canHandle).
    //   2. Otherwise, return the first registered runtime whose canHandle(desc) is true.
    //   3. Returns nullptr if no runtime matches.
    std::shared_ptr<ModuleRuntime> select(const ModuleDescriptor& desc) const;

    // Fan-out terminateAll() to every registered runtime.
    void terminateAll();

    // Aggregate getAllPids() across all runtimes. Later-registered runtimes win on
    // name collision (should not happen in practice).
    std::unordered_map<std::string, int64_t> getAllPids() const;

    // Testing hook: remove all runtimes so a test can install a FakeRuntime
    // without triggering any real Qt subprocess side effects.
    void clearForTests();

private:
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<ModuleRuntime>> m_runtimes;
};

} // namespace LogosCore

#endif // RUNTIME_REGISTRY_H
