#include "dependency_resolver.h"
#include <spdlog/spdlog.h>
#include <unordered_set>
#include <unordered_map>
#include <deque>
#include <string>
#include <vector>

namespace DependencyResolver {

    ResolveResult resolve(const std::vector<std::string>& requested,
                          IsKnownFn isKnown,
                          GetDependenciesFn getDependencies) {
        ResolveResult out;

        std::unordered_set<std::string> modulesToLoad;
        std::deque<std::string> queue(requested.begin(), requested.end());

        while (!queue.empty()) {
            std::string moduleName = queue.front();
            queue.pop_front();

            if (modulesToLoad.count(moduleName))
                continue;

            if (!isKnown(moduleName)) {
                spdlog::warn("Module not found in known modules: {}", moduleName);
                out.missing.push_back(moduleName);
                continue;
            }

            modulesToLoad.insert(moduleName);

            for (const std::string& depName : getDependencies(moduleName)) {
                if (!depName.empty() && !modulesToLoad.count(depName)) {
                    queue.push_back(depName);
                }
            }
        }

        if (!out.missing.empty()) {
            std::string joined;
            for (std::size_t i = 0; i < out.missing.size(); ++i) {
                if (i > 0) joined += ", ";
                joined += out.missing[i];
            }
            spdlog::warn("Missing dependencies detected: {}", joined);
        }

        // Topological sort (Kahn's algorithm)
        std::unordered_map<std::string, std::vector<std::string>> dependents;
        std::unordered_map<std::string, int> inDegree;

        for (const std::string& moduleName : modulesToLoad) {
            if (!inDegree.count(moduleName)) {
                inDegree[moduleName] = 0;
            }

            for (const std::string& depName : getDependencies(moduleName)) {
                if (!depName.empty() && modulesToLoad.count(depName)) {
                    inDegree[moduleName]++;
                    dependents[depName].push_back(moduleName);
                }
            }
        }

        std::deque<std::string> zeroInDegree;

        for (const std::string& moduleName : modulesToLoad) {
            if (inDegree.count(moduleName) == 0 || inDegree.at(moduleName) == 0) {
                zeroInDegree.push_back(moduleName);
            }
        }

        while (!zeroInDegree.empty()) {
            std::string moduleName = zeroInDegree.front();
            zeroInDegree.pop_front();
            out.order.push_back(moduleName);

            auto it = dependents.find(moduleName);
            if (it != dependents.end()) {
                for (const std::string& dependent : it->second) {
                    inDegree[dependent]--;
                    if (inDegree[dependent] == 0) {
                        zeroInDegree.push_back(dependent);
                    }
                }
            }
        }

        if (out.order.size() < modulesToLoad.size()) {
            out.hasCycle = true;
            std::string cycleJoined;
            bool first = true;
            for (const std::string& moduleName : modulesToLoad) {
                bool inResult = false;
                for (const auto& r : out.order) {
                    if (r == moduleName) { inResult = true; break; }
                }
                if (!inResult) {
                    if (!first) cycleJoined += ", ";
                    cycleJoined += moduleName;
                    first = false;
                }
            }
            spdlog::critical("Circular dependency detected involving modules: {}", cycleJoined);
        }

        return out;
    }

}
