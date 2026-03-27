#include "plugin_manager.h"
#include "plugin_registry.h"
#include "dependency_resolver.h"
#include "plugin_launcher.h"
#include <QDebug>
#include <QUuid>
#include <cassert>
#include <cstring>
#include "logos_api.h"
#include "logos_api_client.h"
#include "token_manager.h"

namespace {
    PluginRegistry& registryInstance() {
        static PluginRegistry instance;
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

    void notifyCapabilityModule(const QString& name, const QString& token) {
        if (!registryInstance().isLoaded("capability_module"))
            return;

        TokenManager& tokenManager = TokenManager::instance();
        QString capabilityModuleToken = tokenManager.getToken("capability_module");

        static LogosAPI* s_coreApi = nullptr;
        if (!s_coreApi)
            s_coreApi = new LogosAPI("core");

        LogosAPIClient* client = s_coreApi->getClient("capability_module");
        if (!client->informModuleToken(capabilityModuleToken, name, token)) {
            qWarning() << "Failed to register token with capability module for:" << name;
        }
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

        QString pluginName = registryInstance().processPlugin(path);
        if (pluginName.isEmpty()) {
            qWarning() << "Failed to process plugin:" << path;
            return nullptr;
        }

        QByteArray utf8Data = pluginName.toUtf8();
        char* result = new char[utf8Data.size() + 1];
        strcpy(result, utf8Data.constData());
        return result;
    }

    bool loadPlugin(const char* pluginName) {
        QString name = QString::fromUtf8(pluginName);

        if (!registryInstance().isKnown(name)) {
            qWarning() << "Cannot load unknown plugin:" << name;
            return false;
        }

        if (registryInstance().isLoaded(name)) {
            qWarning() << "Plugin already loaded:" << name;
            return false;
        }

        QString pluginPath = registryInstance().pluginPath(name);

        auto onTerminated = [](const QString& n) {
            registryInstance().markUnloaded(n);
        };

        if (!PluginLauncher::launch(name, pluginPath, registryInstance().pluginsDirs(), onTerminated))
            return false;

        QString authToken = QUuid::createUuid().toString(QUuid::WithoutBraces);

        if (!PluginLauncher::sendToken(name, authToken))
            return false;

        registryInstance().markLoaded(name);

        TokenManager::instance().saveToken(name, authToken);

        notifyCapabilityModule(name, authToken);

        qInfo() << "Plugin loaded:" << name;

        return true;
    }

    bool loadPluginWithDependencies(const char* pluginName) {
        QString name = QString::fromUtf8(pluginName);

        QStringList requested;
        requested.append(name);

        QStringList resolved = DependencyResolver::resolve(
            requested,
            [](const QString& n) { return registryInstance().isKnown(n); },
            [](const QString& n) { return registryInstance().pluginDependencies(n); }
        );

        if (resolved.isEmpty() || !resolved.contains(name)) {
            qWarning() << "Cannot resolve dependencies for:" << name;
            return false;
        }

        bool allSucceeded = true;
        for (const QString& moduleName : resolved) {
            if (registryInstance().isLoaded(moduleName))
                continue;
            if (!loadPlugin(moduleName.toUtf8().constData())) {
                qWarning() << "Failed to load plugin:" << moduleName;
                allSucceeded = false;
            }
        }

        return allSucceeded;
    }

    bool initializeCapabilityModule() {
        if (!registryInstance().isKnown("capability_module"))
            return false;

        if (!loadPlugin("capability_module")) {
            qWarning() << "Failed to load capability module";
            return false;
        }

        return true;
    }

    bool unloadPlugin(const char* pluginName) {
        QString name = QString::fromUtf8(pluginName);

        if (!registryInstance().isLoaded(name)) {
            qWarning() << "Cannot unload plugin (not loaded):" << name;
            return false;
        }

        if (!PluginLauncher::hasProcess(name)) {
            qWarning() << "No process found for plugin:" << name;
            return false;
        }

        PluginLauncher::terminate(name);
        registryInstance().markUnloaded(name);

        qInfo() << "Plugin unloaded:" << name;
        return true;
    }

    void terminateAll() {
        PluginLauncher::terminateAll();
        registryInstance().clearLoaded();
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
        return PluginLauncher::getAllProcessIds();
    }

    QStringList resolveDependencies(const QStringList& requestedModules) {
        return DependencyResolver::resolve(
            requestedModules,
            [](const QString& name) { return registryInstance().isKnown(name); },
            [](const QString& name) { return registryInstance().pluginDependencies(name); }
        );
    }

}
