#ifndef PLUGIN_REGISTRY_H
#define PLUGIN_REGISTRY_H

#include <QString>
#include <QStringList>
#include <QHash>

struct PluginInfo {
    QString path;
    QStringList dependencies;
    bool loaded = false;
};

class PluginRegistry {
public:
    void setPluginsDir(const QString& dir);
    void addPluginsDir(const QString& dir);
    QStringList pluginsDirs() const;

    void discoverInstalledModules();
    QString processPlugin(const QString& pluginPath);

    bool isKnown(const QString& name) const;
    QString pluginPath(const QString& name) const;
    QStringList pluginDependencies(const QString& name) const;
    QStringList knownPluginNames() const;
    void registerPlugin(const QString& name, const QString& path,
                        const QStringList& dependencies = {});
    void registerDependencies(const QString& name, const QStringList& dependencies);

    bool isLoaded(const QString& name) const;
    void markLoaded(const QString& name);
    void markUnloaded(const QString& name);
    QStringList loadedPluginNames() const;
    void clearLoaded();

    void clear();

private:
    QStringList m_pluginsDirs;
    QHash<QString, PluginInfo> m_plugins;
};

#endif // PLUGIN_REGISTRY_H
