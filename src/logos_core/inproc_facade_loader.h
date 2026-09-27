#ifndef INPROC_FACADE_LOADER_H
#define INPROC_FACADE_LOADER_H

// Runs an import's facade in this process when the runtime is single-process:
// libpeering's Facade on the runtime's own lp_* runtime, where elsewhere
// logos_host_remote runs it in a process of its own.

#include "module_loader.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace LogosCore {

class InprocFacadeLoader : public ModuleLoader {
public:
    InprocFacadeLoader();
    ~InprocFacadeLoader() override;

    std::string id() const override { return "inproc-facade"; }
    bool canHandle(const ModuleDescriptor& desc) const override;
    bool load(const ModuleDescriptor& desc,
              std::function<void(const std::string& name)> onTerminated,
              LoadedModuleHandle& out) override;
    bool sendToken(const std::string& name, const std::string& token) override;
    LoadOutcome awaitLoad(const std::string& name, std::chrono::milliseconds timeout) override;
    void terminate(const std::string& name) override;
    void terminateAll() override;
    bool hasModule(const std::string& name) const override;

private:
    struct Entry;
    std::shared_ptr<Entry> take(const std::string& name);

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<Entry>> m_entries;
};

} // namespace LogosCore

#endif // INPROC_FACADE_LOADER_H
