#ifndef COMPOSITE_RUNTIME_H
#define COMPOSITE_RUNTIME_H

#include "module_runtime.h"
#include "module_container.h"
#include "module_loader.h"
#include <memory>

namespace LogosCore {

// Pairs a ModuleContainer (where/how to run) with a ModuleLoader (what to
// load) and presents the combined result as a single ModuleRuntime — the
// interface that RuntimeRegistry and ModuleManager already understand.
class CompositeRuntime : public ModuleRuntime {
public:
    CompositeRuntime(std::shared_ptr<ModuleContainer> container,
                     std::shared_ptr<ModuleLoader> loader,
                     std::string idOverride = "");

    std::string id() const override;
    bool canHandle(const ModuleDescriptor& desc) const override;

    bool load(const ModuleDescriptor& desc,
              std::function<void(const std::string& name)> onTerminated,
              LoadedModuleHandle& out) override;

    bool sendToken(const std::string& name, const std::string& token) override;
    void terminate(const std::string& name) override;
    void terminateAll() override;
    bool hasModule(const std::string& name) const override;
    std::optional<int64_t> pid(const std::string& name) const override;
    std::unordered_map<std::string, int64_t> getAllPids() const override;

    ModuleContainer& container() { return *container_; }
    const ModuleContainer& container() const { return *container_; }

private:
    std::shared_ptr<ModuleContainer> container_;
    std::shared_ptr<ModuleLoader> loader_;
    std::string idOverride_;
};

} // namespace LogosCore

#endif // COMPOSITE_RUNTIME_H
