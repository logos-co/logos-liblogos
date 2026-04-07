#ifndef DEPENDENCY_RESOLVER_H
#define DEPENDENCY_RESOLVER_H

#include <functional>
#include <string>
#include <vector>

namespace DependencyResolver {

    using IsKnownFn = std::function<bool(const std::string&)>;
    using GetDependenciesFn = std::function<std::vector<std::string>(const std::string&)>;

    std::vector<std::string> resolve(const std::vector<std::string>& requested,
                                     IsKnownFn isKnown,
                                     GetDependenciesFn getDependencies);
}

#endif
