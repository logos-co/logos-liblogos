#ifndef SUBPROCESS_MANAGER_H
#define SUBPROCESS_MANAGER_H

// Backward-compatibility shim: SubprocessManager is now composed of
// SubprocessContainer (process lifecycle) + QtPluginRuntime (host resolution).
// Tests and qt_test_adapter.h that reference SubprocessManager by name,
// clearAll(), registerProcess(), startProcess(), ProcessCallbacks etc.
// keep compiling through this header.

#include "subprocess_container.h"
#include "composite_runtime.h"
#include "module_loader.h"
#include "runtimes/runtime_qt/qt_plugin_runtime.h"
#include <memory>

class SubprocessManager : public LogosCore::CompositeRuntime {
public:
    SubprocessManager()
        : CompositeRuntime(std::make_shared<SubprocessContainer>(),
                           std::make_shared<QtPluginRuntime>())
    {}

    // Keep the old id for code that checks it.
    std::string id() const override { return "qt-subprocess"; }

    // Expose ProcessCallbacks under the old name.
    using ProcessCallbacks = SubprocessContainer::ProcessCallbacks;

    // Forward static helpers to SubprocessContainer.
    static bool startProcess(const std::string& name, const std::string& executable,
                             const std::vector<std::string>& arguments,
                             const ProcessCallbacks& callbacks)
    { return SubprocessContainer::startProcess(name, executable, arguments, callbacks); }

    static bool sendTokenToProcess(const std::string& name,
                                    const std::string& token,
                                    int max_wait_ms = 5000)
    { return SubprocessContainer::sendTokenToProcess(name, token, max_wait_ms); }

    static void terminateProcess(const std::string& name)
    { SubprocessContainer::terminateProcess(name); }

    static void terminateAllProcesses()
    { SubprocessContainer::terminateAllProcesses(); }

    static bool hasProcess(const std::string& name)
    { return SubprocessContainer::hasProcess(name); }

    static int64_t getProcessId(const std::string& name)
    { return SubprocessContainer::getProcessId(name); }

    static std::unordered_map<std::string, int64_t> getAllProcessIds()
    { return SubprocessContainer::getAllProcessIds(); }

    static void clearAll()
    { SubprocessContainer::clearAll(); }

    static void registerProcess(const std::string& name)
    { SubprocessContainer::registerProcess(name); }
};

#endif // SUBPROCESS_MANAGER_H
