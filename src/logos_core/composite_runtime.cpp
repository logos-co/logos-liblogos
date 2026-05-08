#include "composite_runtime.h"

namespace LogosCore {

CompositeRuntime::CompositeRuntime(std::shared_ptr<ModuleContainer> container,
                                   std::shared_ptr<ModuleLoader> loader)
    : container_(std::move(container))
    , loader_(std::move(loader))
{}

std::string CompositeRuntime::id() const
{
    return loader_->id() + "+" + container_->id();
}

bool CompositeRuntime::canHandle(const ModuleDescriptor& desc) const
{
    return loader_->canHandle(desc) && container_->canHandle(desc);
}

bool CompositeRuntime::load(const ModuleDescriptor& desc,
                            std::function<void(const std::string& name)> onTerminated,
                            LoadedModuleHandle& out)
{
    std::string host = loader_->resolveHostBinary(desc);
    if (host.empty())
        return false;

    auto args = loader_->buildArguments(desc);
    return container_->launch(desc, host, args, std::move(onTerminated), out);
}

bool CompositeRuntime::sendToken(const std::string& name, const std::string& token)
{
    return container_->sendToken(name, token);
}

void CompositeRuntime::terminate(const std::string& name)
{
    container_->terminate(name);
}

void CompositeRuntime::terminateAll()
{
    container_->terminateAll();
}

bool CompositeRuntime::hasModule(const std::string& name) const
{
    return container_->hasModule(name);
}

std::optional<int64_t> CompositeRuntime::pid(const std::string& name) const
{
    return container_->pid(name);
}

std::unordered_map<std::string, int64_t> CompositeRuntime::getAllPids() const
{
    return container_->getAllPids();
}

} // namespace LogosCore
