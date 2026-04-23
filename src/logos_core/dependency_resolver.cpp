#include "dependency_resolver.h"
#include <spdlog/spdlog.h>
#include <unordered_set>
#include <unordered_map>
#include <deque>
#include <string>
#include <vector>

namespace DependencyResolver {

    std::vector<std::string> resolve(const std::vector<std::string>& requested,
                                     IsKnownFn isKnown,
                                     GetDependenciesFn getDependencies) {
        std::unordered_set<std::string> modulesToLoad;
        std::deque<std::string> queue(requested.begin(), requested.end());
        std::vector<std::string> missingDependencies;

        while (!queue.empty()) {
            std::string moduleName = queue.front();
            queue.pop_front();

            if (modulesToLoad.count(moduleName))
                continue;

            if (!isKnown(moduleName)) {
                spdlog::warn("Module not found in known modules: {}", moduleName);
                missingDependencies.push_back(moduleName);
                continue;
            }

            modulesToLoad.insert(moduleName);

            for (const std::string& depName : getDependencies(moduleName)) {
                if (!depName.empty() && !modulesToLoad.count(depName)) {
                    queue.push_back(depName);
                }
            }
        }

        if (!missingDependencies.empty()) {
            std::string joined;
            for (std::size_t i = 0; i < missingDependencies.size(); ++i) {
                if (i > 0) joined += ", ";
                joined += missingDependencies[i];
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

        std::vector<std::string> result;
        std::deque<std::string> zeroInDegree;

        for (const std::string& moduleName : modulesToLoad) {
            if (inDegree.count(moduleName) == 0 || inDegree.at(moduleName) == 0) {
                zeroInDegree.push_back(moduleName);
            }
        }

        while (!zeroInDegree.empty()) {
            std::string moduleName = zeroInDegree.front();
            zeroInDegree.pop_front();
            result.push_back(moduleName);

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

        if (result.size() < modulesToLoad.size()) {
            std::string cycleJoined;
            bool first = true;
            for (const std::string& moduleName : modulesToLoad) {
                bool inResult = false;
                for (const auto& r : result) {
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

        return result;
    }

}
