#include "plugin_manager.h"
#include "qt/qt_process_manager.h"
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QUuid>
#include <QCoreApplication>
#include <cstring>
#include <cassert>
#include "logos_api.h"
#include "logos_api_client.h"
#include "token_manager.h"
#include "logos_mode.h"
#include <module_lib/module_lib.h>
#include <package_manager_lib.h>

using namespace ModuleLib;

namespace {
    QStringList s_plugins_dirs;
    QStringList s_loaded_plugins;
    QHash<QString, QString> s_known_plugins;
    QHash<QString, QJsonObject> s_plugin_metadata;
}

namespace PluginManager {

    void setPluginsDir(const char* plugins_dir) {
        assert(plugins_dir != nullptr);
        s_plugins_dirs.clear();
        s_plugins_dirs.append(QString(plugins_dir));
        qInfo() << "Custom plugins directory set to:" << s_plugins_dirs.first();
    }

    void addPluginsDir(const char* plugins_dir) {
        assert(plugins_dir != nullptr);
        QString dir = QString(plugins_dir);
        if (s_plugins_dirs.contains(dir)) return;
        s_plugins_dirs.append(dir);
        qDebug() << "Added plugins directory:" << dir;
    }

    QStringList getPluginsDirs() {
        return s_plugins_dirs;
    }

    // Delegate to PackageManagerLib for platform variant selection and plugin scanning
    static PackageManagerLib& packageManagerInstance() {
        static PackageManagerLib instance;
        return instance;
    }

    void discoverInstalledModules() {
        // Configure the package manager with the directories set via the C API
        PackageManagerLib& pm = packageManagerInstance();
        if (!s_plugins_dirs.isEmpty()) {
            pm.setEmbeddedModulesDirectory(s_plugins_dirs.first().toStdString());
            for (int i = 1; i < s_plugins_dirs.size(); ++i) {
                pm.setUserModulesDirectory(s_plugins_dirs[i].toStdString());
            }
        }

        std::string jsonStr = pm.getInstalledModules();
        QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(jsonStr));
        QJsonArray modules = doc.array();

        for (const QJsonValue& val : modules) {
            QJsonObject mod = val.toObject();
            QString name = mod.value("name").toString();
            QString mainFilePath = mod.value("mainFilePath").toString();

            if (name.isEmpty() || mainFilePath.isEmpty())
                continue;

            // Process plugin binary to extract Qt metadata and register
            QString pluginName = processPlugin(mainFilePath);
            if (pluginName.isEmpty()) {
                qWarning() << "Failed to process plugin (no metadata or invalid):" << mainFilePath;
            } else {
                qDebug() << "Discovered module:" << pluginName << "at" << mainFilePath;
            }
        }

        qDebug() << "Total known plugins after discovery:" << s_known_plugins.size();
        qDebug() << "Known plugin names:" << s_known_plugins.keys();
    }

    void terminateAll() {
        QtProcessManager::terminateAll();
        s_loaded_plugins.clear();
    }

    QString processPlugin(const QString &pluginPath) {
        auto metadataOpt = LogosModule::extractMetadata(pluginPath);
        if (!metadataOpt) {
            qWarning() << "No metadata found for plugin:" << pluginPath;
            return QString();
        }

        const ModuleMetadata& metadata = *metadataOpt;
        if (!metadata.isValid()) {
            qWarning() << "Plugin name not specified in metadata for:" << pluginPath;
            return QString();
        }

        s_known_plugins.insert(metadata.name, pluginPath);
        s_plugin_metadata.insert(metadata.name, metadata.rawMetadata);

        return metadata.name;
    }

    char* processPluginCStr(const char* pluginPath) {
        QString path = QString::fromUtf8(pluginPath);
        qDebug() << "Processing plugin file:" << path;

        QString pluginName = processPlugin(path);
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
        const QString name = QString::fromUtf8(pluginName);
        qDebug() << "Attempting to load plugin by name:" << name;

        if (!s_known_plugins.contains(name)) {
            qWarning() << "Plugin not found among known plugins:" << name;
            return false;
        }

        QString pluginPath = s_known_plugins.value(name);

        qDebug() << "Loading plugin:" << name << "from path:" << pluginPath << "in separate process";

        if (isPluginLoaded(name)) {
            qWarning() << "Plugin already loaded:" << name;
            return false;
        }

        // Find the logos_host executable with multiple strategies
        QString logosHostPath;
        // 1) Environment override
        QByteArray envPathBytes = qgetenv("LOGOS_HOST_PATH");
        if (!envPathBytes.isEmpty()) {
            logosHostPath = QString::fromUtf8(envPathBytes);
        }
        // 2) Default next to Electron/host executable
        if (logosHostPath.isEmpty()) {
            logosHostPath = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/logos_host");
        }

        // 3) Fallback relative to plugins directory (../../logos-liblogos/build/bin/logos_host)
        if (!QFile::exists(logosHostPath)) {
            if (!s_plugins_dirs.isEmpty()) {
                QDir pluginsDirCandidate(s_plugins_dirs.first());
                QString candidate = QDir::cleanPath(pluginsDirCandidate.absoluteFilePath("../bin/logos_host"));
                if (QFile::exists(candidate)) {
                    logosHostPath = candidate;
                }
            }
        }

        qDebug() << "Logos host path (resolved):" << logosHostPath;

        if (!QFile::exists(logosHostPath)) {
            qCritical() << "logos_host executable not found at:" << logosHostPath;
            qCritical() << "Set environment variable LOGOS_HOST_PATH to the absolute path of logos_host or ensure it is next to the Electron executable or under ../bin from the plugins directory.";
            return false;
        }

        QStringList arguments;
        arguments << "--name" << name;
        arguments << "--path" << pluginPath;

        qDebug() << "Starting logos_host with arguments:" << arguments;

        QtProcessManager::ProcessCallbacks callbacks;

        callbacks.onFinished = [](const QString& pluginName, int exitCode, bool crashed) {
            Q_UNUSED(exitCode);
            if (crashed) {
                qCritical() << "Plugin process crashed:" << pluginName << "- terminating core with error";
                exit(1);
            }
            s_loaded_plugins.removeAll(pluginName);
        };

        callbacks.onError = [](const QString& pluginName, bool crashed) {
            if (crashed) {
                qCritical() << "Plugin process crashed:" << pluginName << "- terminating core with error";
                exit(1);
            }
        };

        callbacks.onOutput = [](const QString& pluginName, const QString& line, bool isStderr) {
            if (isStderr) {
                qCritical() << "[LOGOS_HOST" << pluginName << "] STDERR:" << line;
            } else if (line.contains("qrc:") || line.contains("Warning:") || line.contains("WARNING:")) {
                qWarning() << "[LOGOS_HOST" << pluginName << "]:" << line;
            } else if (line.contains("Critical:") || line.contains("FAILED:") || line.contains("ERROR:")) {
                qCritical() << "[LOGOS_HOST" << pluginName << "]:" << line;
            } else {
                qDebug() << "[LOGOS_HOST" << pluginName << "]:" << line;
            }
        };

        if (!QtProcessManager::startProcess(name, logosHostPath, arguments, callbacks)) {
            return false;
        }

        // Generate auth token and send via IPC
        QUuid authToken = QUuid::createUuid();
        QString authTokenString = authToken.toString(QUuid::WithoutBraces);
        qDebug() << "Generated auth token:" << authTokenString;

        if (!QtProcessManager::sendToken(name, authTokenString)) {
            return false;
        }

        qDebug() << "Auth token sent securely to plugin:" << name;

        s_loaded_plugins.append(name);

        TokenManager& tokenManager = TokenManager::instance();
        tokenManager.saveToken(name, authTokenString);

        // Inform capability module about the new module token
        if (s_loaded_plugins.contains("capability_module")) {
            qDebug() << "Informing capability module about new module token for:" << name;

            QString capabilityModuleToken = tokenManager.getToken("capability_module");
            qDebug() << "Capability module token:" << capabilityModuleToken;

            static LogosAPI* s_coreApi = nullptr;
            if (!s_coreApi)
                s_coreApi = new LogosAPI("core");

            LogosAPIClient* client = s_coreApi->getClient("capability_module");
            bool success = client->informModuleToken(capabilityModuleToken, name, authTokenString);
            if (success) {
                qDebug() << "Successfully informed capability module about token for:" << name;
            } else {
                qWarning() << "Failed to inform capability module about token for:" << name;
            }
        } else {
            qDebug() << "Capability module not loaded, skipping token notification";
        }

        qDebug() << "Plugin" << name << "is now running in separate process";
        qDebug() << "Remote registry URL for this plugin: local:logos_" << name;

        return true;
    }

    bool loadPluginWithDependencies(const char* pluginName) {
        const QString name = QString::fromUtf8(pluginName);
        QStringList requestedModules;
        requestedModules.append(name);

        QStringList resolvedModules = resolveDependencies(requestedModules);

        if (resolvedModules.isEmpty() || !resolvedModules.contains(name)) {
            qWarning() << "Cannot load plugin: plugin not found:" << name;
            return false;
        }

        bool allSucceeded = true;
        for (const QString& moduleName : resolvedModules) {
            if (isPluginLoaded(moduleName)) {
                qDebug() << "Plugin already loaded, skipping:" << moduleName;
                continue;
            }
            if (!loadPlugin(moduleName.toUtf8().constData())) {
                qWarning() << "Failed to load module:" << moduleName;
                allSucceeded = false;
            }
        }

        return allSucceeded;
    }

    QString currentPlatformVariant() {
#if defined(Q_OS_MAC)
    #if defined(Q_PROCESSOR_ARM)
        return "darwin-arm64";
    #else
        return "darwin-x86_64";
    #endif
#elif defined(Q_OS_LINUX)
    #if defined(Q_PROCESSOR_X86_64)
        return "linux-x86_64";
    #elif defined(Q_PROCESSOR_ARM_64)
        return "linux-arm64";
    #else
        return "linux-x86";
    #endif
#else
        return "unknown";
#endif
    }

    bool initializeCapabilityModule() {
        qDebug() << "\n=== Initializing Capability Module ===";

        if (!s_known_plugins.contains("capability_module")) {
            qDebug() << "Capability module not found in known plugins, skipping initialization";
            return false;
        }

        qDebug() << "Capability module found, attempting to load...";

        bool success = loadPlugin("capability_module");

        if (!success) {
            qDebug() << "Failed to load capability module";
            return false;
        }

        qDebug() << "Capability module loaded successfully";

        return true;
    }

    bool unloadPlugin(const char* pluginName) {
        const QString name = QString::fromUtf8(pluginName);
        qDebug() << "Attempting to unload plugin by name:" << name;

        if (!s_loaded_plugins.contains(name)) {
            qWarning() << "Plugin not loaded, cannot unload:" << name;
            qDebug() << "Loaded plugins:" << s_loaded_plugins;
            return false;
        }

        if (!QtProcessManager::hasProcess(name)) {
            qWarning() << "No process found for plugin:" << name;
            return false;
        }

        QtProcessManager::terminateProcess(name);

        s_loaded_plugins.removeAll(name);
        
        qDebug() << "Successfully unloaded plugin:" << name;
        return true;
    }

    char** getLoadedPluginsCStr() {
        int count = s_loaded_plugins.size();
        
        if (count == 0) {
            char** result = new char*[1];
            result[0] = nullptr;
            return result;
        }
        
        char** result = new char*[count + 1];
        
        for (int i = 0; i < count; ++i) {
            QByteArray utf8Data = s_loaded_plugins[i].toUtf8();
            result[i] = new char[utf8Data.size() + 1];
            strcpy(result[i], utf8Data.constData());
        }
        
        result[count] = nullptr;
        
        return result;
    }

    char** getKnownPluginsCStr() {
        QStringList knownPlugins = s_known_plugins.keys();
        int count = knownPlugins.size();
        
        if (count == 0) {
            qWarning() << "No known plugins to return";
            char** result = new char*[1];
            result[0] = nullptr;
            return result;
        }
        
        char** result = new char*[count + 1];
        
        for (int i = 0; i < count; ++i) {
            QByteArray utf8Data = knownPlugins[i].toUtf8();
            result[i] = new char[utf8Data.size() + 1];
            strcpy(result[i], utf8Data.constData());
        }
        
        result[count] = nullptr;
        
        return result;
    }

    QStringList getLoadedPlugins() {
        return s_loaded_plugins;
    }

    QHash<QString, QString> getKnownPlugins() {
        return s_known_plugins;
    }

    bool isPluginLoaded(const QString& name) {
        return s_loaded_plugins.contains(name);
    }

    bool isPluginKnown(const QString& name) {
        return s_known_plugins.contains(name);
    }

    QStringList resolveDependencies(const QStringList& requestedModules) {
        qDebug() << "Resolving dependencies for modules:" << requestedModules;
        
        QSet<QString> modulesToLoad;
        QStringList queue = requestedModules;
        QStringList missingDependencies;
        
        while (!queue.isEmpty()) {
            QString moduleName = queue.takeFirst();
            
            if (modulesToLoad.contains(moduleName)) {
                continue;
            }
            
            if (!s_known_plugins.contains(moduleName)) {
                qWarning() << "Module not found in known plugins:" << moduleName;
                missingDependencies.append(moduleName);
                continue;
            }
            
            modulesToLoad.insert(moduleName);
            
            if (s_plugin_metadata.contains(moduleName)) {
                QJsonObject metadata = s_plugin_metadata.value(moduleName);
                QJsonArray deps = metadata.value("dependencies").toArray();
                for (const QJsonValue& dep : deps) {
                    QString depName = dep.toString();
                    if (!depName.isEmpty() && !modulesToLoad.contains(depName)) {
                        queue.append(depName);
                    }
                }
            }
        }
        
        if (!missingDependencies.isEmpty()) {
            qWarning() << "Missing dependencies detected:" << missingDependencies;
        }
        
        QHash<QString, QStringList> dependents;
        QHash<QString, int> inDegree;
        
        for (const QString& moduleName : modulesToLoad) {
            if (!inDegree.contains(moduleName)) {
                inDegree[moduleName] = 0;
            }
            
            if (s_plugin_metadata.contains(moduleName)) {
                QJsonObject metadata = s_plugin_metadata.value(moduleName);
                QJsonArray deps = metadata.value("dependencies").toArray();
                for (const QJsonValue& dep : deps) {
                    QString depName = dep.toString();
                    if (!depName.isEmpty() && modulesToLoad.contains(depName)) {
                        inDegree[moduleName]++;
                        dependents[depName].append(moduleName);
                    }
                }
            }
        }
        
        QStringList result;
        QStringList zeroInDegree;
        
        for (const QString& moduleName : modulesToLoad) {
            if (inDegree.value(moduleName, 0) == 0) {
                zeroInDegree.append(moduleName);
            }
        }
        
        while (!zeroInDegree.isEmpty()) {
            QString moduleName = zeroInDegree.takeFirst();
            result.append(moduleName);
            
            for (const QString& dependent : dependents.value(moduleName)) {
                inDegree[dependent]--;
                if (inDegree[dependent] == 0) {
                    zeroInDegree.append(dependent);
                }
            }
        }
        
        if (result.size() < modulesToLoad.size()) {
            QStringList cycleModules;
            for (const QString& moduleName : modulesToLoad) {
                if (!result.contains(moduleName)) {
                    cycleModules.append(moduleName);
                }
            }
            qCritical() << "Circular dependency detected involving modules:" << cycleModules;
        }
        
        qDebug() << "Resolved load order:" << result;
        return result;
    }

    void clearState() {
        qDebug() << "Clearing all plugin state";
        
        s_plugins_dirs.clear();
        s_loaded_plugins.clear();
        s_known_plugins.clear();
        s_plugin_metadata.clear();
        
        QtProcessManager::clearAll();

        qDebug() << "Plugin state cleared";
    }

    void addKnownPlugin(const QString& name, const QString& path) {
        qDebug() << "Adding known plugin:" << name << "at path:" << path;
        s_known_plugins.insert(name, path);
    }

    void addPluginMetadata(const QString& name, const QJsonObject& metadata) {
        s_plugin_metadata.insert(name, metadata);
    }

    QHash<QString, qint64> getPluginProcessIds() {
        return QtProcessManager::getAllProcessIds();
    }

    void registerLoadedPlugin(const QString& name) {
        if (!s_loaded_plugins.contains(name)) {
            s_loaded_plugins.append(name);
        }
    }

}
