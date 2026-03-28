#ifndef PLUGIN_LAUNCHER_H
#define PLUGIN_LAUNCHER_H

#include <QString>
#include <QStringList>
#include <QHash>
#include <functional>

namespace PluginLauncher {
    using OnTerminatedFn = std::function<void(const QString& name)>;

    bool launch(const QString& name, const QString& pluginPath,
                const QStringList& pluginsDirs, OnTerminatedFn onTerminated);

    // Type-aware launch: for gRPC modules, launches the executable directly
    // with --socket and --name arguments instead of going through logos_host.
    bool launch(const QString& name, const QString& pluginPath,
                const QStringList& pluginsDirs, const QString& moduleType,
                const QString& socketPath, OnTerminatedFn onTerminated);

    bool sendToken(const QString& name, const QString& token);
    void terminate(const QString& name);
    void terminateAll();
    bool hasProcess(const QString& name);
    QHash<QString, qint64> getAllProcessIds();
}

#endif // PLUGIN_LAUNCHER_H
