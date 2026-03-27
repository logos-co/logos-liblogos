#include "plugin_manager.h"
#include "plugin_registry.h"
#include "plugin_loader.h"
#include "dependency_resolver.h"
#include <QDebug>
#include <cassert>
#include <cstring>

namespace {
    PluginRegistry& registryInstance() {
        static PluginRegistry instance;
        return instance;
    }

    PluginLoader& loaderInstance() {
        static PluginLoader instance(registryInstance());
        return instance;
    }

    char** toNullTerminatedArray(const QStringList& list) {
        int count = list.size();
        if (count == 0) {
            char** result = new char*[1];
            result[0] = nullptr;
            return result;
        }

        char** result = new char*[count + 1];
        for (int i = 0; i < count; ++i) {
            QByteArray utf8Data = list[i].toUtf8();
            result[i] = new char[utf8Data.size() + 1];
            strcpy(result[i], utf8Data.constData());
        }
        result[count] = nullptr;
        return result;
    }
}

namespace PluginManager {

    PluginRegistry& registry() {
        return registryInstance();
    }

    void setPluginsDir(const char* plugins_dir) {
        assert(plugins_dir != nullptr);
        registryInstance().setPluginsDir(QString(plugins_dir));
    }

    void addPluginsDir(const char* plugins_dir) {
        assert(plugins_dir != nullptr);
        registryInstance().addPluginsDir(QString(plugins_dir));
    }

    void discoverInstalledModules() {
        registryInstance().discoverInstalledModules();
    }

    QString processPlugin(const QString& pluginPath) {
        return registryInstance().processPlugin(pluginPath);
    }

    char* processPluginCStr(const char* pluginPath) {
        QString path = QString::fromUtf8(pluginPath);
        qDebug() << "Processing plugin file:" << path;

        QString pluginName = registryInstance().processPlugin(path);
        if (pluginName.isEmpty()) {
            qWarning() << "Failed to process plugin file:" << path;
            return nullptr;
        }

        QByteArray utf8Data = pluginName.toUtf8();
        char* result = new char[utf8Data.size() + 1];
        strcpy(result, utf8Data.constData());
        return result;
    }

    bool loadPlugin(const char* pluginName) {
        return loaderInstance().loadPlugin(QString::fromUtf8(pluginName));
    }

    bool loadPluginWithDependencies(const char* pluginName) {
        return loaderInstance().loadPluginWithDependencies(QString::fromUtf8(pluginName));
    }

    bool initializeCapabilityModule() {
        return loaderInstance().initializeCapabilityModule();
    }

    bool unloadPlugin(const char* pluginName) {
        return loaderInstance().unloadPlugin(QString::fromUtf8(pluginName));
    }

    void terminateAll() {
        loaderInstance().terminateAll();
    }

    char** getLoadedPluginsCStr() {
        return toNullTerminatedArray(registryInstance().loadedPluginNames());
    }

    char** getKnownPluginsCStr() {
        QStringList known = registryInstance().knownPluginNames();
        if (known.isEmpty()) {
            qWarning() << "No known plugins to return";
        }
        return toNullTerminatedArray(known);
    }

    bool isPluginLoaded(const QString& name) {
        return registryInstance().isLoaded(name);
    }

    QHash<QString, qint64> getPluginProcessIds() {
        return loaderInstance().getPluginProcessIds();
    }

    QStringList resolveDependencies(const QStringList& requestedModules) {
        return DependencyResolver::resolve(
            requestedModules,
            [](const QString& name) { return registryInstance().isKnown(name); },
            [](const QString& name) { return registryInstance().pluginMetadata(name); }
        );
    }

}
