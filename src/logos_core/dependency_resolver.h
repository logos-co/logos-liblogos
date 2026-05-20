#ifndef DEPENDENCY_RESOLVER_H
#define DEPENDENCY_RESOLVER_H

#include <string>
#include <vector>
#include <functional>

namespace DependencyResolver {

    using IsKnownFn = std::function<bool(const std::string&)>;
    using GetDependenciesFn = std::function<std::vector<std::string>(const std::string&)>;

    // Result of dependency resolution. `order` is a topological sort of
    // the reachable, known modules. `missing` lists dependency names
    // that were referenced but not known to the registry. `hasCycle` is
    // true when the reachable graph contains a cycle (Kahn's algorithm
    // could not consume all nodes). Callers decide policy: load paths
    // treat !ok() as a hard failure; teardown paths may ignore it.
    struct ResolveResult {
        std::vector<std::string> order;
        std::vector<std::string> missing;
        bool hasCycle = false;

        bool ok() const { return missing.empty() && !hasCycle; }
    };

    ResolveResult resolve(const std::vector<std::string>& requested,
                          IsKnownFn isKnown,
                          GetDependenciesFn getDependencies);
}

#endif // DEPENDENCY_RESOLVER_H
