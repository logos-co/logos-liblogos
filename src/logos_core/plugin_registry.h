#ifndef PLUGIN_REGISTRY_H
#define PLUGIN_REGISTRY_H

#include <QString>
#include <QStringList>
#include <QHash>
#include <nlohmann/json.hpp>

class PluginRegistry {
public:
    void setPluginsDir(const QString& dir);
    void addPluginsDir(const QString& dir);
    QStringList pluginsDirs() const;

    void discoverInstalledModules();
    QString processPlugin(const QString& pluginPath);

    bool isKnown(const QString& name) const;
    QString pluginPath(const QString& name) const;
    nlohmann::json pluginMetadata(const QString& name) const;
    QStringList knownPluginNames() const;
    void registerPlugin(const QString& name, const QString& path);
    void registerMetadata(const QString& name, const nlohmann::json& metadata);

    bool isLoaded(const QString& name) const;
    void markLoaded(const QString& name);
    void markUnloaded(const QString& name);
    QStringList loadedPluginNames() const;
    void clearLoaded();

    void clear();

private:
    QStringList m_pluginsDirs;
    QHash<QString, QString> m_knownPlugins;
    QHash<QString, nlohmann::json> m_pluginMetadata;
    QStringList m_loadedPlugins;
};

#endif // PLUGIN_REGISTRY_H
