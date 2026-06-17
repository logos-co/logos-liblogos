#ifndef SUBPROCESS_MANAGER_H
#define SUBPROCESS_MANAGER_H

// Test-only helper: composes SubprocessContainer (process lifecycle, from
// logos-container-subprocess) + QtPluginFormatLoader (host resolution, liblogos)
// into a CompositeModuleLoader, and forwards the container's static
// process-management helpers under the SubprocessManager name. Lives in tests/
// (not src/) deliberately: production code constructs the container directly in
// module_manager.cpp, so a dependency on a *specific* container belongs only in
// the tests that exercise it — the module-loader layer must stay
// container-agnostic. Used by qt_test_adapter.h, test_subprocess_manager.cpp,
// and test_module_loader_abstraction.cpp.

#include <logos_container_subprocess/subprocess_container.h>
#include "composite_module_loader.h"
#include <logos_module_loader/module_format_loader.h>
#include <logos_module_loader_qt/qt_plugin_format_loader.h>
#include <memory>

class SubprocessManager : public LogosCore::CompositeModuleLoader {
public:
    SubprocessManager()
        : CompositeModuleLoader(std::make_shared<SubprocessContainer>(),
                                std::make_shared<QtPluginFormatLoader>())
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
