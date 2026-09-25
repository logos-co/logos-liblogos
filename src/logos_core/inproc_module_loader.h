#ifndef INPROC_MODULE_LOADER_H
#define INPROC_MODULE_LOADER_H

// Runs trusted native modules in this process ("direct_dynamic" in the spec).
// In-process means full trust and shared fate, so it is decided before the image
// is opened: bundled, stamped eligible by the builder, and placed here by policy.

#include "module_loader.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

struct lp_provider;

namespace LogosCore {

// The embedder's placement policy: {"default":"subprocess"|"inproc",
// "modules":{"<name>":"subprocess"|"inproc"}, "single_process":bool}.
struct PlacementPolicy {
    bool defaultInProcess = false;
    std::unordered_map<std::string, bool> modules; // name -> in-process
    bool singleProcess = false;                    // every module in-process, or it does not load
};

bool parsePlacementPolicy(const std::string& json, PlacementPolicy& out, std::string& error);

struct PlacementDecision {
    bool inProcess = false;
    bool refused = false; // single_process, and this module cannot run here
    std::string reason;
};

// `sidecar` is the module's stamped metadata; only a bundled, eligible
// native module is ever placed here.
PlacementDecision decidePlacement(const std::string& name, const nlohmann::json& sidecar,
                                  const std::string& format, bool bundled,
                                  const PlacementPolicy& policy);

class InprocModuleLoader : public ModuleLoader {
public:
    InprocModuleLoader();
    ~InprocModuleLoader() override;

    std::string id() const override { return "inproc"; }
    bool canHandle(const ModuleDescriptor& desc) const override;
    bool load(const ModuleDescriptor& desc,
              std::function<void(const std::string& name)> onTerminated,
              LoadedModuleHandle& out) override;
    bool sendToken(const std::string& name, const std::string& token) override;
    LoadOutcome awaitLoad(const std::string& name, std::chrono::milliseconds timeout) override;
    void terminate(const std::string& name) override;
    void terminateAll() override;
    bool hasModule(const std::string& name) const override;

    // The image and provider of a module running here, for the runtime's own
    // interfaces to it; nullptr when it is not.
    void* symbolOf(const std::string& name, const char* symbol) const;
    lp_provider* providerOf(const std::string& name) const;

private:
    struct Entry;
    std::shared_ptr<Entry> take(const std::string& name);

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<Entry>> m_entries;
};

} // namespace LogosCore

#endif // INPROC_MODULE_LOADER_H
