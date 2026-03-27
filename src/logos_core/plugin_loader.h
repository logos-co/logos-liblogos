#ifndef PLUGIN_LOADER_H
#define PLUGIN_LOADER_H

#include <QString>
#include <QHash>

class PluginRegistry;

class PluginLoader {
public:
    explicit PluginLoader(PluginRegistry& registry);

    bool loadPlugin(const QString& name);
    bool loadPluginWithDependencies(const QString& name);
    bool initializeCapabilityModule();
    bool unloadPlugin(const QString& name);
    void terminateAll();
    QHash<QString, qint64> getPluginProcessIds();

private:
    QString resolveLogosHostPath();
    void notifyCapabilityModule(const QString& name, const QString& token);

    PluginRegistry& m_registry;
};

#endif // PLUGIN_LOADER_H
