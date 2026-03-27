#include "plugin_manager.h"
#include "plugin_registry.h"
#include "dependency_resolver.h"
#include "qt/qt_process_manager.h"
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QUuid>
#include <QCoreApplication>
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

    QString resolveLogosHostPath() {
        QString logosHostPath;

        QByteArray envPathBytes = qgetenv("LOGOS_HOST_PATH");
        if (!envPathBytes.isEmpty()) {
            logosHostPath = QString::fromUtf8(envPathBytes);
        }

        if (logosHostPath.isEmpty()) {
            logosHostPath = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/logos_host");
        }

        if (!QFile::exists(logosHostPath)) {
            QStringList dirs = registryInstance().pluginsDirs();
            if (!dirs.isEmpty()) {
                QDir pluginsDirCandidate(dirs.first());
                QString candidate = QDir::cleanPath(pluginsDirCandidate.absoluteFilePath("../bin/logos_host"));
                if (QFile::exists(candidate)) {
                    logosHostPath = candidate;
                }
            }
        }

        if (!QFile::exists(logosHostPath)) {
            qCritical() << "logos_host not found at:" << logosHostPath
                         << "- set LOGOS_HOST_PATH or place it next to the executable";
            return QString();
        }

        return logosHostPath;
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

        QString logosHostPath = resolveLogosHostPath();
        if (logosHostPath.isEmpty())
            return false;

        std::vector<std::string> arguments = {
            "--name", name.toStdString(),
            "--path", pluginPath.toStdString()
        };

        QtProcessManager::ProcessCallbacks callbacks;

        callbacks.onFinished = [](const std::string& pName, int exitCode, bool crashed) {
            Q_UNUSED(exitCode);
            QString qName = QString::fromStdString(pName);
            if (crashed) {
                qCritical() << "Plugin process crashed:" << qName;
                exit(1);
            }
            registryInstance().markUnloaded(qName);
        };

        callbacks.onError = [](const std::string& pName, bool crashed) {
            if (crashed) {
                qCritical() << "Plugin process crashed:" << QString::fromStdString(pName);
                exit(1);
            }
        };

        callbacks.onOutput = [](const std::string& pName, const std::string& line, bool isStderr) {
            QString qName = QString::fromStdString(pName);
            QString qLine = QString::fromStdString(line);
            if (isStderr) {
                qCritical() << "[" << qName << "]" << qLine;
            } else if (qLine.contains("Warning:") || qLine.contains("WARNING:")) {
                qWarning() << "[" << qName << "]" << qLine;
            } else if (qLine.contains("Critical:") || qLine.contains("FAILED:") || qLine.contains("ERROR:")) {
                qCritical() << "[" << qName << "]" << qLine;
            }
        };

        if (!QtProcessManager::startProcess(name.toStdString(), logosHostPath.toStdString(), arguments, callbacks)) {
            return false;
        }

        QUuid authToken = QUuid::createUuid();
        QString authTokenString = authToken.toString(QUuid::WithoutBraces);

        if (!QtProcessManager::sendToken(name.toStdString(), authTokenString.toStdString())) {
            return false;
        }

        registryInstance().markLoaded(name);

        TokenManager& tokenManager = TokenManager::instance();
        tokenManager.saveToken(name, authTokenString);

        notifyCapabilityModule(name, authTokenString);

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

        if (!QtProcessManager::hasProcess(name.toStdString())) {
            qWarning() << "No process found for plugin:" << name;
            return false;
        }

        QtProcessManager::terminateProcess(name.toStdString());
        registryInstance().markUnloaded(name);

        qInfo() << "Plugin unloaded:" << name;
        return true;
    }

    void terminateAll() {
        QtProcessManager::terminateAll();
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
        auto stdMap = QtProcessManager::getAllProcessIds();
        QHash<QString, qint64> result;
        for (const auto& [name, pid] : stdMap) {
            result.insert(QString::fromStdString(name), pid);
        }
        return result;
    }

    QStringList resolveDependencies(const QStringList& requestedModules) {
        return DependencyResolver::resolve(
            requestedModules,
            [](const QString& name) { return registryInstance().isKnown(name); },
            [](const QString& name) { return registryInstance().pluginDependencies(name); }
        );
    }

}
