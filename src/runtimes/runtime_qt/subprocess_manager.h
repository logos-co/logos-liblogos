#ifndef SUBPROCESS_MANAGER_H
#define SUBPROCESS_MANAGER_H

#include "module_runtime.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>

class SubprocessManager : public LogosCore::ModuleRuntime {
public:
    struct ProcessCallbacks {
        std::function<void(const std::string& name, int exitCode, bool crashed)> onFinished;
        std::function<void(const std::string& name, bool crashed)> onError;
        std::function<void(const std::string& name, const std::string& line, bool isStderr)> onOutput;
    };

    // -- ModuleRuntime interface --
    std::string id() const override { return "qt-subprocess"; }
    bool canHandle(const LogosCore::ModuleDescriptor& desc) const override;
    bool load(const LogosCore::ModuleDescriptor& desc,
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
    // Connect to the child's QtTokenReceiver Unix socket and write the
    // auth token. The child binds the socket inside `setupModule` →
    // `receiveAuthToken`, which runs *after* a non-trivial chain of
    // child-side initialisation (dynamic loader, Qt platform bring-up,
    // CLI11 parse, plugin loadFromPath). This call retries until the
    // socket appears or the budget runs out.
    //
    // `max_wait_ms` is the total budget; defaults to 5 seconds, which
    // comfortably covers cold-start child initialisation on realistic
    // hardware. The previous 900 ms budget (10 × 100 ms) was tight
    // enough that races against the child binding its socket were
    // observable in the wild — see tests/test_subprocess_manager.cpp.
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

#endif // SUBPROCESS_MANAGER_H
