#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include <QString>
#include <QStringList>
#include <QHash>

class PluginRegistry;

namespace PluginManager {
    PluginRegistry& registry();

    void setPluginsDir(const char* plugins_dir);
    void addPluginsDir(const char* plugins_dir);

    void discoverInstalledModules();

    QString processPlugin(const QString& pluginPath);
    char* processPluginCStr(const char* pluginPath);
    bool loadPlugin(const char* pluginName);
    bool loadPluginWithDependencies(const char* pluginName);
    bool initializeCapabilityModule();
    bool unloadPlugin(const char* pluginName);
    void terminateAll();

    char** getLoadedPluginsCStr();
    char** getKnownPluginsCStr();

    bool isPluginLoaded(const QString& name);
    QHash<QString, qint64> getPluginProcessIds();

    QStringList resolveDependencies(const QStringList& requestedModules);
}

#endif // PLUGIN_MANAGER_H
