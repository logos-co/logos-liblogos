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
    bool sendToken(const QString& name, const QString& token);
    void terminate(const QString& name);
    void terminateAll();
    bool hasProcess(const QString& name);
    QHash<QString, qint64> getAllProcessIds();
}

#endif // PLUGIN_LAUNCHER_H
