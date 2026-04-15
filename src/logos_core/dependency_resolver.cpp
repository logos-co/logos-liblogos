#include "dependency_resolver.h"
#include <spdlog/spdlog.h>
#include <QSet>
#include <QHash>

namespace DependencyResolver {

    QStringList resolve(const QStringList& requested,
                        IsKnownFn isKnown,
                        GetDependenciesFn getDependencies) {
        QSet<QString> modulesToLoad;
        QStringList queue = requested;
        QStringList missingDependencies;

        while (!queue.isEmpty()) {
            QString moduleName = queue.takeFirst();

            if (modulesToLoad.contains(moduleName))
                continue;

            if (!isKnown(moduleName)) {
                spdlog::warn("Module not found in known plugins: {}", moduleName.toStdString());
                missingDependencies.append(moduleName);
                continue;
            }

            modulesToLoad.insert(moduleName);

            for (const QString& depName : getDependencies(moduleName)) {
                if (!depName.isEmpty() && !modulesToLoad.contains(depName)) {
                    queue.append(depName);
                }
            }
        }

        if (!missingDependencies.isEmpty()) {
            spdlog::warn("Missing dependencies detected: {}", missingDependencies.join(", ").toStdString());
        }

        // Topological sort (Kahn's algorithm)
        QHash<QString, QStringList> dependents;
        QHash<QString, int> inDegree;

        for (const QString& moduleName : modulesToLoad) {
            if (!inDegree.contains(moduleName)) {
                inDegree[moduleName] = 0;
            }

            for (const QString& depName : getDependencies(moduleName)) {
                if (!depName.isEmpty() && modulesToLoad.contains(depName)) {
                    inDegree[moduleName]++;
                    dependents[depName].append(moduleName);
                }
            }
        }

        QStringList result;
        QStringList zeroInDegree;

        for (const QString& moduleName : modulesToLoad) {
            if (inDegree.value(moduleName, 0) == 0) {
                zeroInDegree.append(moduleName);
            }
        }

        while (!zeroInDegree.isEmpty()) {
            QString moduleName = zeroInDegree.takeFirst();
            result.append(moduleName);

            for (const QString& dependent : dependents.value(moduleName)) {
                inDegree[dependent]--;
                if (inDegree[dependent] == 0) {
                    zeroInDegree.append(dependent);
                }
            }
        }

        if (result.size() < modulesToLoad.size()) {
            QStringList cycleModules;
            for (const QString& moduleName : modulesToLoad) {
                if (!result.contains(moduleName)) {
                    cycleModules.append(moduleName);
                }
            }
            spdlog::critical("Circular dependency detected involving modules: {}", cycleModules.join(", ").toStdString());
        }

        return result;
    }

}
