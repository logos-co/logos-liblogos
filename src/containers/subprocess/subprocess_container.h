#ifndef SUBPROCESS_CONTAINER_H
#define SUBPROCESS_CONTAINER_H

#include "module_container.h"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class SubprocessContainer : public LogosCore::ModuleContainer {
public:
    struct ProcessCallbacks {
        std::function<void(const std::string& name, int exitCode, bool crashed)> onFinished;
        std::function<void(const std::string& name, bool crashed)> onError;
        std::function<void(const std::string& name, const std::string& line, bool isStderr)> onOutput;
    };

    // -- ModuleContainer interface --
    std::string id() const override { return "subprocess"; }
    bool canHandle(const LogosCore::ModuleDescriptor& desc) const override;
    bool launch(const LogosCore::ModuleDescriptor& desc,
                const std::string& hostBinary,
                const std::vector<std::string>& args,
                std::function<void(const std::string&)> onTerminated,
                LogosCore::LoadedModuleHandle& out) override;
    bool sendToken(const std::string& name, const std::string& token) override;
    void terminate(const std::string& name) override;
    void terminateAll() override;
    bool hasModule(const std::string& name) const override;
    std::optional<int64_t> pid(const std::string& name) const override;
    std::unordered_map<std::string, int64_t> getAllPids() const override;

    // -- Low-level static process management (used by tests / qt_test_adapter) --
    static bool startProcess(const std::string& name, const std::string& executable,
                             const std::vector<std::string>& arguments, const ProcessCallbacks& callbacks);
    static bool sendTokenToProcess(const std::string& name,
                                    const std::string& token,
                                    int max_wait_ms = 5000);
    static void terminateProcess(const std::string& name);
    static void terminateAllProcesses();
    static bool hasProcess(const std::string& name);
    static int64_t getProcessId(const std::string& name);
    static std::unordered_map<std::string, int64_t> getAllProcessIds();
    static void clearAll();
    static void registerProcess(const std::string& name);
};

#endif // SUBPROCESS_CONTAINER_H
