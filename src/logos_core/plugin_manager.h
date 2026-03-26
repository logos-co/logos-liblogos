#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include <QString>
#include <QStringList>
#include <QHash>
#include <QJsonObject>

class QProcess;

namespace PluginManager {
    void setPluginsDir(const char* plugins_dir);
    void addPluginsDir(const char* plugins_dir);
    QStringList getPluginsDirs();

    void discoverInstalledModules();

    QString processPlugin(const QString& pluginPath);
    char* processPluginCStr(const char* pluginPath);
    bool loadPlugin(const char* pluginName);
    bool loadPluginWithDependencies(const char* pluginName);
    QStringList findPlugins(const QString& pluginsDir);
    bool initializeCapabilityModule();
    bool unloadPlugin(const char* pluginName);
    void terminateAll();

    char** getLoadedPluginsCStr();
    char** getKnownPluginsCStr();

    QStringList getLoadedPlugins();
    QHash<QString, QString> getKnownPlugins();
    bool isPluginLoaded(const QString& name);
    bool isPluginKnown(const QString& name);
    QHash<QString, qint64> getPluginProcessIds();

    QStringList resolveDependencies(const QStringList& requestedModules);

    void clearState();
    void addKnownPlugin(const QString& name, const QString& path);
    void addPluginMetadata(const QString& name, const QJsonObject& metadata);

    // Register a plugin as loaded without spawning a process.
    // If process is non-null it is tracked for termination/stats.
    void registerLoadedPlugin(const QString& name, QProcess* process = nullptr);
}

#endif // PLUGIN_MANAGER_H
