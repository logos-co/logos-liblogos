#ifndef SANDBOX_CONTAINER_H
#define SANDBOX_CONTAINER_H

#include "module_container.h"
#include "containers/subprocess/subprocess_container.h"
#include <mutex>
#include <string>
#include <unordered_map>

class SandboxContainer : public LogosCore::ModuleContainer {
public:
    std::string id() const override;
    bool canHandle(const LogosCore::ModuleDescriptor& desc) const override;

    bool launch(const LogosCore::ModuleDescriptor& desc,
                const std::string& hostBinary,
                const std::vector<std::string>& args,
                std::function<void(const std::string& name)> onTerminated,
                LogosCore::LoadedModuleHandle& out) override;

    bool sendToken(const std::string& name, const std::string& token) override;
    void terminate(const std::string& name) override;
    void terminateAll() override;
    bool hasModule(const std::string& name) const override;
    std::optional<int64_t> pid(const std::string& name) const override;
    std::unordered_map<std::string, int64_t> getAllPids() const override;

    static std::string generateProfile(const LogosCore::ModuleDescriptor& desc,
                                       const std::string& hostBinary);

private:
    SubprocessContainer subprocess_;

    mutable std::mutex profilesMutex_;
    std::unordered_map<std::string, std::string> profilePaths_;

    static std::string writeProfileFile(const std::string& moduleName,
                                        const std::string& profileContent);

    void cleanupProfile(const std::string& name);
};

#endif // SANDBOX_CONTAINER_H
