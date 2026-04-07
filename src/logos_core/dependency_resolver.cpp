#include "dependency_resolver.h"
#include "logos_logging.h"
#include <algorithm>
#include <deque>
#include <set>
#include <unordered_map>

namespace DependencyResolver {

    std::vector<std::string> resolve(const std::vector<std::string>& requested,
                                     IsKnownFn isKnown,
                                     GetDependenciesFn getDependencies)
    {
        std::set<std::string> modulesToLoad;
        std::deque<std::string> queue(requested.begin(), requested.end());
        std::vector<std::string> missingDependencies;

        while (!queue.empty()) {
            std::string moduleName = queue.front();
            queue.pop_front();

            if (modulesToLoad.count(moduleName))
                continue;

            if (!isKnown(moduleName)) {
                logos_log_warn("Module not found in known plugins: {}", moduleName);
                missingDependencies.push_back(moduleName);
                continue;
            }

            modulesToLoad.insert(moduleName);

            for (const std::string& depName : getDependencies(moduleName)) {
                if (!depName.empty() && !modulesToLoad.count(depName))
                    queue.push_back(depName);
            }
        }

        if (!missingDependencies.empty()) {
            logos_log_warn("Missing dependencies detected (count={})", missingDependencies.size());
        }

        std::unordered_map<std::string, std::vector<std::string>> dependents;
        std::unordered_map<std::string, int> inDegree;

        for (const std::string& moduleName : modulesToLoad) {
            if (!inDegree.count(moduleName))
                inDegree[moduleName] = 0;

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
            if (inDegree[moduleName] == 0)
                zeroInDegree.push_back(moduleName);
        }

        while (!zeroInDegree.empty()) {
            std::string moduleName = zeroInDegree.front();
            zeroInDegree.pop_front();
            result.push_back(moduleName);

            for (const std::string& dependent : dependents[moduleName]) {
                inDegree[dependent]--;
                if (inDegree[dependent] == 0)
                    zeroInDegree.push_back(dependent);
            }
        }

        if (result.size() < modulesToLoad.size()) {
            std::vector<std::string> cycleModules;
            for (const std::string& moduleName : modulesToLoad) {
                if (std::find(result.begin(), result.end(), moduleName) == result.end())
                    cycleModules.push_back(moduleName);
            }
            std::string cycleList;
            for (size_t i = 0; i < cycleModules.size(); ++i) {
                if (i)
                    cycleList += ", ";
                cycleList += cycleModules[i];
            }
            logos_log_critical("Circular dependency detected involving modules: {}", cycleList);
        }

        return result;
    }

}
