#ifndef QT_SUBPROCESS_RUNTIME_H
#define QT_SUBPROCESS_RUNTIME_H

#include "../../logos_core/module_runtime.h"

namespace LogosCore {

// ModuleRuntime implementation for the "qt-subprocess" strategy:
//  - spawns a logos_host child process per module
//  - delivers an auth token via a Unix-domain socket (logos_token_<name>)
//  - module communicates back via Qt Remote Objects over a local socket
class QtSubprocessRuntime : public ModuleRuntime {
public:
    std::string id() const override { return "qt-subprocess"; }

    // Handles qt-plugin format (or unspecified format, which defaults to qt-plugin).
    bool canHandle(const ModuleDescriptor& desc) const override;

    bool load(const ModuleDescriptor& desc,
              std::function<void(const std::string&)> onTerminated,
              LoadedModuleHandle& out) override;

    bool sendToken(const std::string& name, const std::string& token) override;
    void terminate(const std::string& name) override;
    void terminateAll() override;
    bool hasModule(const std::string& name) const override;
    std::optional<int64_t> pid(const std::string& name) const override;
    std::unordered_map<std::string, int64_t> getAllPids() const override;
};

} // namespace LogosCore

#endif // QT_SUBPROCESS_RUNTIME_H
