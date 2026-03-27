#include "dependency_resolver.h"
#include <QDebug>
#include <QSet>
#include <QHash>

namespace DependencyResolver {

    QStringList resolve(const QStringList& requested,
                        IsKnownFn isKnown,
                        GetDependenciesFn getDependencies) {
        qDebug() << "Resolving dependencies for modules:" << requested;

        QSet<QString> modulesToLoad;
        QStringList queue = requested;
        QStringList missingDependencies;

        while (!queue.isEmpty()) {
            QString moduleName = queue.takeFirst();

            if (modulesToLoad.contains(moduleName))
                continue;

            if (!isKnown(moduleName)) {
                qWarning() << "Module not found in known plugins:" << moduleName;
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
            qWarning() << "Missing dependencies detected:" << missingDependencies;
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
            qCritical() << "Circular dependency detected involving modules:" << cycleModules;
        }

        qDebug() << "Resolved load order:" << result;
        return result;
    }

}
