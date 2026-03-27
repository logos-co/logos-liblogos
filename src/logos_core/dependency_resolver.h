#ifndef DEPENDENCY_RESOLVER_H
#define DEPENDENCY_RESOLVER_H

#include <QString>
#include <QStringList>
#include <functional>

namespace DependencyResolver {

    using IsKnownFn = std::function<bool(const QString&)>;
    using GetDependenciesFn = std::function<QStringList(const QString&)>;

    QStringList resolve(const QStringList& requested,
                        IsKnownFn isKnown,
                        GetDependenciesFn getDependencies);
}

#endif // DEPENDENCY_RESOLVER_H
