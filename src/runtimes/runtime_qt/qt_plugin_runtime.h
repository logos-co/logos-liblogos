#ifndef QT_PLUGIN_RUNTIME_H
#define QT_PLUGIN_RUNTIME_H

#include "module_loader.h"
#include <string>
#include <vector>

// Parent-side logic for the Qt-plugin module format: knows how to locate the
// logos_host_qt binary and build the CLI arguments it expects.
class QtPluginRuntime : public LogosCore::ModuleLoader {
public:
    std::string id() const override { return "qt-plugin"; }

    bool canHandle(const LogosCore::ModuleDescriptor& desc) const override;

    std::string resolveHostBinary(const LogosCore::ModuleDescriptor& desc) const override;

    std::vector<std::string> buildArguments(const LogosCore::ModuleDescriptor& desc) const override;
};

#endif // QT_PLUGIN_RUNTIME_H
