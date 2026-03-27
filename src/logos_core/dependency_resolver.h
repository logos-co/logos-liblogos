#ifndef DEPENDENCY_RESOLVER_H
#define DEPENDENCY_RESOLVER_H

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <functional>

namespace DependencyResolver {

    using IsKnownFn = std::function<bool(const QString&)>;
    using GetMetadataFn = std::function<QJsonObject(const QString&)>;

    QStringList resolve(const QStringList& requested,
                        IsKnownFn isKnown,
                        GetMetadataFn getMetadata);
}

#endif // DEPENDENCY_RESOLVER_H
