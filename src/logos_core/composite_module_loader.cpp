#include "composite_module_loader.h"

namespace LogosCore {

CompositeModuleLoader::CompositeModuleLoader(std::shared_ptr<ModuleContainer> container,
                                             std::shared_ptr<ModuleFormatLoader> loader)
    : container_(std::move(container))
    , loader_(std::move(loader))
{}

std::string CompositeModuleLoader::id() const
{
    return loader_->id() + "+" + container_->id();
}

bool CompositeModuleLoader::canHandle(const ModuleDescriptor& desc) const
{
    return loader_->canHandle(desc) && container_->canHandle(desc);
}

bool CompositeModuleLoader::load(const ModuleDescriptor& desc,
                                 std::function<void(const std::string& name)> onTerminated,
                                 LoadedModuleHandle& out)
{
    std::string host = loader_->resolveHostBinary(desc);
    if (host.empty())
        return false;

    auto args = loader_->buildArguments(desc);
    // The flag is the capability check: a host too old to read a configuration
    // refuses it and exits, rather than starting the module without one.
    const bool configured = takesConfiguration(desc);
    if (configured) {
        args.push_back("--configuration-source");
        args.push_back("stdin");
    }
    {
        std::lock_guard<std::mutex> lock(configuredMutex_);
        if (configured) configured_.insert(desc.name);
        else configured_.erase(desc.name);
    }
    return container_->launch(desc, host, args, std::move(onTerminated), out);
}

bool CompositeModuleLoader::sendToken(const std::string& name, const std::string& token)
{
    return sendStartupInput(name, token, std::nullopt);
}

bool CompositeModuleLoader::sendStartupInput(const std::string& name, const std::string& token,
                                             const std::optional<std::string>& configuration)
{
    bool launchedConfigured = false;
    {
        std::lock_guard<std::mutex> lock(configuredMutex_);
        launchedConfigured = configured_.count(name) > 0;
    }
    // Both or neither: a host told to read a configuration waits for its line.
    if (launchedConfigured != configuration.has_value()) return false;
    if (!configuration) return container_->sendToken(name, token);
    if (configuration->find_first_of("\r\n") != std::string::npos) return false;
    return container_->sendToken(name, token + "\n" + *configuration);
}

LoadOutcome CompositeModuleLoader::awaitLoad(const std::string& name,
                                             std::chrono::milliseconds timeout)
{
    return container_->awaitLoad(name, timeout);
}

void CompositeModuleLoader::terminate(const std::string& name)
{
    container_->terminate(name);
}

void CompositeModuleLoader::terminateAll()
{
    container_->terminateAll();
}

bool CompositeModuleLoader::hasModule(const std::string& name) const
{
    return container_->hasModule(name);
}

std::optional<int64_t> CompositeModuleLoader::pid(const std::string& name) const
{
    return container_->pid(name);
}

std::unordered_map<std::string, int64_t> CompositeModuleLoader::getAllPids() const
{
    return container_->getAllPids();
}

} // namespace LogosCore
