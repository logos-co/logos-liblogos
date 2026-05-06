#ifndef MODULE_LOADER_H
#define MODULE_LOADER_H

#include "module_runtime.h"
#include <string>
#include <vector>

namespace LogosCore {

// Abstract interface for module type / format strategies.
// A loader knows how to resolve the host binary and build the CLI arguments
// for a particular module format (Qt plugin, extism/WASM, etc.) but has no
// opinion about *where* the module runs — that is the container's job.
class ModuleLoader {
public:
    virtual ~ModuleLoader() = default;

    virtual std::string id() const = 0;

    virtual bool canHandle(const ModuleDescriptor& desc) const = 0;

    // Resolve the path to the host binary that loads this module format.
    virtual std::string resolveHostBinary(const ModuleDescriptor& desc) const = 0;

    // Build the CLI arguments the host binary needs to load the described module.
    virtual std::vector<std::string> buildArguments(const ModuleDescriptor& desc) const = 0;
};

} // namespace LogosCore

#endif // MODULE_LOADER_H
