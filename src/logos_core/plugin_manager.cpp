#include "plugin_manager.h"
#include "logos_core_internal.h"
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QUuid>
#include <QTimer>
#include <QThread>
#include <QProcess>
#include <QLocalSocket>
#include <QLocalServer>
#include <cstring>
#include <cassert>
#include "logos_api_client.h"
#include "logos_core_client.h"
#include "token_manager.h"
#include "module_lib.h"

using namespace ModuleLib;

namespace PluginManager {

    QString processPlugin(const QString &pluginPath) {
        qDebug() << "\n------------------------------------------";
        qDebug() << "Processing plugin from:" << pluginPath;

        // Use module_lib to extract metadata
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

        qDebug() << "Plugin Metadata:";
        qDebug() << " - Name:" << metadata.name;
        qDebug() << " - Version:" << metadata.version;
        qDebug() << " - Description:" << metadata.description;
        qDebug() << " - Author:" << metadata.author;
        qDebug() << " - Type:" << metadata.type;

        if (!metadata.dependencies.isEmpty()) {
            qDebug() << " - Dependencies:";
            for (const QString &dependency : metadata.dependencies) {
                qDebug() << "   *" << dependency;
                if (!g_loaded_plugins.contains(dependency)) {
                    qWarning() << "Required dependency not loaded:" << dependency;
                }
            }
        }

        g_known_plugins.insert(metadata.name, pluginPath);
        qDebug() << "Added to known plugins: " << metadata.name << " -> " << pluginPath;
        
        // Store the raw metadata for compatibility with existing code
        g_plugin_metadata.insert(metadata.name, metadata.rawMetadata);
        qDebug() << "Stored metadata for plugin:" << metadata.name;
        
        return metadata.name;
    }

    bool loadPlugin(const QString &pluginName) {
        qDebug() << "Attempting to load plugin by name:" << pluginName;

        if (!g_known_plugins.contains(pluginName)) {
            qWarning() << "Plugin not found among known plugins:" << pluginName;
            return false;
        }

        QString pluginPath = g_known_plugins.value(pluginName);

        qDebug() << "Loading plugin:" << pluginName << "from path:" << pluginPath << "in separate process";

        // Check if plugin is already loaded
        if (g_plugin_processes.contains(pluginName)) {
            qWarning() << "Plugin already loaded:" << pluginName;
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
            if (!g_plugins_dirs.isEmpty()) {
                QDir pluginsDirCandidate(g_plugins_dirs.first());
                QString candidate = QDir::cleanPath(pluginsDirCandidate.absoluteFilePath("../bin/logos_host"));
                if (QFile::exists(candidate)) {
                    logosHostPath = candidate;
                }
            }
        }

        qDebug() << "Logos host path (resolved):" << logosHostPath;

        // Check if logos_host exists
        if (!QFile::exists(logosHostPath)) {
            qCritical() << "logos_host executable not found at:" << logosHostPath;
            qCritical() << "Set environment variable LOGOS_HOST_PATH to the absolute path of logos_host or ensure it is next to the Electron executable or under ../bin from the plugins directory.";
            return false;
        }

        // Create a new process for the plugin
        QProcess* process = new QProcess();
        
        // Set up the process to capture output (merge stdout and stderr)
        process->setProcessChannelMode(QProcess::MergedChannels);
        
        // Set up arguments for logos_host
        QStringList arguments;
        arguments << "--name" << pluginName;
        arguments << "--path" << pluginPath;

        qDebug() << "Starting logos_host with arguments:" << arguments;

        // Start the process
        process->start(logosHostPath, arguments);

        if (!process->waitForStarted(5000)) { // Wait up to 5 seconds for the process to start
            qCritical() << "Failed to start logos_host process:" << process->errorString();
            delete process;
            return false;
        }

        qDebug() << "Logos host process started successfully for plugin:" << pluginName;
        qDebug() << "Process ID:" << process->processId();

        // Set up IPC to securely send the auth token
        QString socketName = QString("logos_token_%1").arg(pluginName);
        QLocalSocket* tokenSocket = new QLocalSocket();

        // Try to connect to the socket (with retries since the process might need time to set up)
        bool connected = false;
        for (int i = 0; i < 10; ++i) { // Try for up to 1 second
            tokenSocket->connectToServer(socketName);
            if (tokenSocket->waitForConnected(100)) {
                connected = true;
                break;
            }
            QThread::msleep(100); // Wait 100ms before retry
        }

        if (!connected) {
            qCritical() << "Failed to connect to token socket for plugin:" << pluginName;
            tokenSocket->deleteLater();
            process->terminate();
            delete process;
            return false;
        }

        // generate a guid and print it
        QUuid authToken = QUuid::createUuid();
        QString authTokenString = authToken.toString(QUuid::WithoutBraces);
        qDebug() << "Generated auth token:" << authTokenString;

        // Send the auth token securely via IPC
        QByteArray tokenData = QString(authTokenString).toUtf8();
        tokenSocket->write(tokenData);
        tokenSocket->waitForBytesWritten(1000);
        tokenSocket->disconnectFromServer();
        tokenSocket->deleteLater();

        qDebug() << "Auth token sent securely to plugin:" << pluginName;

        // Store the process
        g_plugin_processes.insert(pluginName, process);

        // Add the plugin name to our loaded plugins list
        g_loaded_plugins.append(pluginName);

        TokenManager& tokenManager = TokenManager::instance();
        tokenManager.saveToken(pluginName, authTokenString);

        // call InformModuleToken on Capability Module
        if (g_loaded_plugins.contains("capability_module")) {
            qDebug() << "Informing capability module about new module token for:" << pluginName;

            QString capabilityModuleToken = tokenManager.getToken("capability_module");
            qDebug() << "Capability module token:" << capabilityModuleToken;

            static LogosCoreClient* s_coreClient = nullptr;
            if (!s_coreClient)
                s_coreClient = new LogosCoreClient();

            LogosAPIClient* client = s_coreClient->clientFor("capability_module");
            bool success = client->informModuleToken(capabilityModuleToken, pluginName, authTokenString);
            if (success) {
                qDebug() << "Successfully informed capability module about token for:" << pluginName;
            } else {
                qWarning() << "Failed to inform capability module about token for:" << pluginName;
            }
        } else {
            qDebug() << "Capability module not loaded, skipping token notification";
        }

        // Connect to process finished signal for cleanup
        QObject::connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                        [pluginName, process](int exitCode, QProcess::ExitStatus exitStatus) {
                            qDebug() << "Plugin process finished:" << pluginName 
                                    << "Exit code:" << exitCode 
                                    << "Exit status:" << exitStatus;
                            
                            // TODO: This is temporary and later needs a mechanism to restart the process
                            if (exitStatus == QProcess::CrashExit) {
                                qCritical() << "Plugin process crashed:" << pluginName << "- terminating core with error";
                                exit(1);
                            }
                            
                            // Remove from our tracking lists
                            g_plugin_processes.remove(pluginName);
                            g_loaded_plugins.removeAll(pluginName);
                            
                            // Clean up the process object
                            process->deleteLater();
                        });

        // Connect to error signal
        QObject::connect(process, &QProcess::errorOccurred,
                        [pluginName](QProcess::ProcessError error) {
                            qCritical() << "Plugin process error for" << pluginName << ":" << error;
                            
                            // TODO: This is temporary and later needs a mechanism to restart the process
                            if (error == QProcess::Crashed) {
                                qCritical() << "Plugin process crashed:" << pluginName << "- terminating core with error";
                                exit(1);
                            }
                        });

        // Connect to output signals to forward logs from logos_host to main process
        QObject::connect(process, &QProcess::readyReadStandardOutput,
                        [pluginName, process]() {
                            QByteArray output = process->readAllStandardOutput();
                            if (!output.isEmpty()) {
                                // Forward logs to main process with plugin prefix
                                QString logLine = QString::fromUtf8(output).trimmed();
                                QStringList lines = logLine.split('\n', Qt::SkipEmptyParts);
                                for (const QString &line : lines) {
                                    // Parse the Qt log level from the line and forward appropriately
                                    if (line.contains("qrc:") || line.contains("Warning:") || line.contains("WARNING:")) {
                                        qWarning() << "[LOGOS_HOST" << pluginName << "]:" << line;
                                    } else if (line.contains("Critical:") || line.contains("FAILED:") || line.contains("ERROR:")) {
                                        qCritical() << "[LOGOS_HOST" << pluginName << "]:" << line;
                                    } else {
                                        qDebug() << "[LOGOS_HOST" << pluginName << "]:" << line;
                                    }
                                }
                            }
                        });

        // Connect to stderr output (in case channel mode changes)
        QObject::connect(process, &QProcess::readyReadStandardError,
                        [pluginName, process]() {
                            QByteArray output = process->readAllStandardError();
                            if (!output.isEmpty()) {
                                // Forward error logs to main process with plugin prefix
                                QString logLine = QString::fromUtf8(output).trimmed();
                                QStringList lines = logLine.split('\n', Qt::SkipEmptyParts);
                                for (const QString &line : lines) {
                                    qCritical() << "[LOGOS_HOST" << pluginName << "] STDERR:" << line;
                                }
                            }
                        });

        qDebug() << "Plugin" << pluginName << "is now running in separate process";
        qDebug() << "Remote registry URL for this plugin: local:logos_" << pluginName;
        
        return true;
    }

    void loadAndProcessPlugin(const QString &pluginPath) {
        QString pluginName = processPlugin(pluginPath);

        if (pluginName.isEmpty()) {
            qWarning() << "Failed to process plugin:" << pluginPath;
            return;
        }

        loadPlugin(pluginName);
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

    QStringList platformVariantsToTry() {
        QString primary = currentPlatformVariant();
        QStringList variants;
        variants << primary;

        if (primary == "linux-x86_64") {
            variants << "linux-amd64";
        } else if (primary == "linux-amd64") {
            variants << "linux-x86_64";
        } else if (primary == "linux-arm64") {
            variants << "linux-aarch64";
        } else if (primary == "linux-aarch64") {
            variants << "linux-arm64";
        }

#ifndef LOGOS_PORTABLE_BUILD
        QStringList devVariants;
        for (const QString &variant : variants) {
            devVariants << (variant + "-dev");
        }
        variants = devVariants;
#endif

        return variants;
    }

    QStringList findPlugins(const QString &pluginsDir) {
        QDir dir(pluginsDir);
        QStringList plugins;
        
        qDebug() << "Searching for plugins in:" << dir.absolutePath();
        
        if (!dir.exists()) {
            qWarning() << "Plugins directory does not exist:" << dir.absolutePath();
            return plugins;
        }
        
        QStringList subdirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        qDebug() << "Subdirectories found:" << subdirs;
        
        for (const QString &subdir : subdirs) {
            QDir pluginDir(dir.absoluteFilePath(subdir));
            QString manifestPath = pluginDir.absoluteFilePath("manifest.json");
            
            if (!QFile::exists(manifestPath)) {
                qDebug() << "No manifest.json in" << pluginDir.absolutePath() << ", skipping";
                continue;
            }
            
            QFile manifestFile(manifestPath);
            if (!manifestFile.open(QIODevice::ReadOnly)) {
                qWarning() << "Failed to open manifest:" << manifestPath;
                continue;
            }
            
            QJsonParseError parseError;
            QJsonDocument doc = QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
            manifestFile.close();
            
            if (parseError.error != QJsonParseError::NoError) {
                qWarning() << "Failed to parse manifest:" << manifestPath << parseError.errorString();
                continue;
            }
            
            QJsonObject manifest = doc.object();
            QJsonValue mainValue = manifest.value("main");
            
            if (!mainValue.isObject()) {
                qWarning() << "Manifest 'main' field is not an object:" << manifestPath;
                continue;
            }
            
            QJsonObject mainObj = mainValue.toObject();
            QStringList variants = platformVariantsToTry();
            QString mainLib;
            for (const QString &variant : variants) {
                mainLib = mainObj.value(variant).toString();
                if (!mainLib.isEmpty()) break;
            }
            
            if (mainLib.isEmpty()) {
                qDebug() << "No entry for platform variants" << variants << "in manifest:" << manifestPath;
                continue;
            }
            
            QString libPath = pluginDir.absoluteFilePath(mainLib);
            if (!QFile::exists(libPath)) {
                qWarning() << "Main library not found:" << libPath;
                continue;
            }
            
            plugins.append(libPath);
            qDebug() << "Found plugin:" << libPath;
        }
        
        return plugins;
    }

    bool initializeCapabilityModule() {
        qDebug() << "\n=== Initializing Capability Module ===";

        // Check if capability_module is available in known plugins
        if (!g_known_plugins.contains("capability_module")) {
            qDebug() << "Capability module not found in known plugins, skipping initialization";
            return false;
        }

        qDebug() << "Capability module found, attempting to load...";

        // Load the capability module
        bool success = loadPlugin("capability_module");

        if (!success) {
            qDebug() << "Failed to load capability module";
            return false;
        }

        qDebug() << "Capability module loaded successfully";

        return true;
    }

    bool unloadPlugin(const QString& pluginName) {
        qDebug() << "Attempting to unload plugin by name:" << pluginName;

        // Check if plugin is loaded
        if (!g_loaded_plugins.contains(pluginName)) {
            qWarning() << "Plugin not loaded, cannot unload:" << pluginName;
            qDebug() << "Loaded plugins:" << g_loaded_plugins;
            return false;
        }

        // Check if we have a process for this plugin
        if (!g_plugin_processes.contains(pluginName)) {
            qWarning() << "No process found for plugin:" << pluginName;
            return false;
        }

        // Get the process
        QProcess* process = g_plugin_processes.value(pluginName);
        
        qDebug() << "Terminating plugin process for:" << pluginName;
        
        // Terminate the process gracefully
        process->terminate();
        
        // Wait for the process to finish, with a timeout
        if (!process->waitForFinished(5000)) {
            qWarning() << "Process did not terminate gracefully, killing it";
            process->kill();
            process->waitForFinished(2000);
        }

        // Remove from our tracking structures
        g_plugin_processes.remove(pluginName);
        g_loaded_plugins.removeAll(pluginName);
        
        // The process will be cleaned up by the signal handler
        qDebug() << "Successfully unloaded plugin:" << pluginName;
        return true;
    }

    char** getLoadedPluginsCStr() {
        int count = g_loaded_plugins.size();
        
        if (count == 0) {
            // Return an array with just a NULL terminator
            char** result = new char*[1];
            result[0] = nullptr;
            return result;
        }
        
        // Allocate memory for the array of strings
        char** result = new char*[count + 1];  // +1 for null terminator
        
        // Copy each plugin name
        for (int i = 0; i < count; ++i) {
            QByteArray utf8Data = g_loaded_plugins[i].toUtf8();
            result[i] = new char[utf8Data.size() + 1];
            strcpy(result[i], utf8Data.constData());
        }
        
        // Null-terminate the array
        result[count] = nullptr;
        
        return result;
    }

    char** getKnownPluginsCStr() {
        // Get the keys from the hash (plugin names)
        QStringList knownPlugins = g_known_plugins.keys();
        int count = knownPlugins.size();
        
        if (count == 0) {
            qWarning() << "No known plugins to return";
            // Return an array with just a NULL terminator
            char** result = new char*[1];
            result[0] = nullptr;
            return result;
        }
        
        // Allocate memory for the array of strings
        char** result = new char*[count + 1];  // +1 for null terminator
        
        // Copy each plugin name
        for (int i = 0; i < count; ++i) {
            QByteArray utf8Data = knownPlugins[i].toUtf8();
            result[i] = new char[utf8Data.size() + 1];
            strcpy(result[i], utf8Data.constData());
        }
        
        // Null-terminate the array
        result[count] = nullptr;
        
        return result;
    }

    QStringList getLoadedPlugins() {
        return g_loaded_plugins;
    }

    QHash<QString, QString> getKnownPlugins() {
        return g_known_plugins;
    }

    bool isPluginLoaded(const QString& name) {
        return g_loaded_plugins.contains(name);
    }

    bool isPluginKnown(const QString& name) {
        return g_known_plugins.contains(name);
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
            
            if (!g_known_plugins.contains(moduleName)) {
                qWarning() << "Module not found in known plugins:" << moduleName;
                missingDependencies.append(moduleName);
                continue;
            }
            
            modulesToLoad.insert(moduleName);
            
            if (g_plugin_metadata.contains(moduleName)) {
                QJsonObject metadata = g_plugin_metadata.value(moduleName);
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
            
            if (g_plugin_metadata.contains(moduleName)) {
                QJsonObject metadata = g_plugin_metadata.value(moduleName);
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
        
        g_plugins_dirs.clear();
        g_loaded_plugins.clear();
        g_known_plugins.clear();
        g_plugin_metadata.clear();
        
        // Terminate and clean up all plugin processes
        for (auto it = g_plugin_processes.begin(); it != g_plugin_processes.end(); ++it) {
            QProcess* process = it.value();
            if (process) {
                process->terminate();
                process->waitForFinished(1000);
                delete process;
            }
        }
        g_plugin_processes.clear();

        qDebug() << "Plugin state cleared";
    }

    void addKnownPlugin(const QString& name, const QString& path) {
        qDebug() << "Adding known plugin:" << name << "at path:" << path;
        g_known_plugins.insert(name, path);
    }

}
