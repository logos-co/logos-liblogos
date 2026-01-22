#include "plugin_manager.h"
#include "logos_core_internal.h"
#include <QPluginLoader>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QUuid>
#include <QTimer>
#include <QThread>
#ifndef Q_OS_IOS
#include <QProcess>
#include <QLocalSocket>
#endif
#include <QLocalServer>
#include <cstring>
#include "../common/interface.h"
#include "core_manager/core_manager.h"
#include "logos_api.h"
#include "logos_api_provider.h"
#include "logos_api_client.h"
#include "token_manager.h"
#include "logos_mode.h"

namespace PluginManager {

QString processPlugin(const QString &pluginPath)
{
    qDebug() << "\n------------------------------------------";
    qDebug() << "Processing plugin from:" << pluginPath;

    // Load the plugin metadata without instantiating the plugin
    QPluginLoader loader(pluginPath);

    // Read the metadata
    QJsonObject metadata = loader.metaData();
    if (metadata.isEmpty()) {
        qWarning() << "No metadata found for plugin:" << pluginPath;
        return QString();
    }

    // Read our custom metadata from the metadata.json file
    QJsonObject customMetadata = metadata.value("MetaData").toObject();
    if (customMetadata.isEmpty()) {
        qWarning() << "No custom metadata found for plugin:" << pluginPath;
        return QString();
    }

    QString pluginName = customMetadata.value("name").toString();
    if (pluginName.isEmpty()) {
        qWarning() << "Plugin name not specified in metadata for:" << pluginPath;
        return QString();
    }

    qDebug() << "Plugin Metadata:";
    qDebug() << " - Name:" << pluginName;
    qDebug() << " - Version:" << customMetadata.value("version").toString();
    qDebug() << " - Description:" << customMetadata.value("description").toString();
    qDebug() << " - Author:" << customMetadata.value("author").toString();
    qDebug() << " - Type:" << customMetadata.value("type").toString();
    
    // Log capabilities
    QJsonArray capabilities = customMetadata.value("capabilities").toArray();
    if (!capabilities.isEmpty()) {
        qDebug() << " - Capabilities:";
        for (const QJsonValue &cap : capabilities) {
            qDebug() << "   *" << cap.toString();
        }
    }

    // Check dependencies
    QJsonArray dependencies = customMetadata.value("dependencies").toArray();
    if (!dependencies.isEmpty()) {
        qDebug() << " - Dependencies:";
        for (const QJsonValue &dep : dependencies) {
            QString dependency = dep.toString();
            qDebug() << "   *" << dependency;
            if (!g_loaded_plugins.contains(dependency)) {
                qWarning() << "Required dependency not loaded:" << dependency;
            }
        }
    }

    // Store the plugin in the known plugins hash
    g_known_plugins.insert(pluginName, pluginPath);
    qDebug() << "Added to known plugins: " << pluginName << " -> " << pluginPath;
    
    return pluginName;
}

bool loadPlugin(const QString &pluginName)
{
    qDebug() << "Attempting to load plugin by name:" << pluginName;
    
    if (!g_known_plugins.contains(pluginName)) {
        qWarning() << "Plugin not found among known plugins:" << pluginName;
        return false;
    }

    QString pluginPath = g_known_plugins.value(pluginName);

    if (LogosModeConfig::isLocal()) {
        qDebug() << "Loading plugin:" << pluginName << "from path:" << pluginPath << "in-process (Local mode)";

        // Check if plugin is already loaded
        if (g_local_plugin_apis.contains(pluginName)) {
            qWarning() << "Plugin already loaded (Local mode):" << pluginName;
            return false;
        }

        // Load the plugin using QPluginLoader
        QPluginLoader loader(pluginPath);
        QObject* plugin = loader.instance();

        if (!plugin) {
            qCritical() << "Failed to load plugin (Local mode):" << loader.errorString();
            return false;
        }

        qDebug() << "Plugin loaded successfully (Local mode)";

        // Cast to the base PluginInterface
        PluginInterface* basePlugin = qobject_cast<PluginInterface*>(plugin);
        if (!basePlugin) {
            qCritical() << "Plugin does not implement the PluginInterface (Local mode)";
            return false;
        }

        // Verify that the plugin name matches
        if (pluginName != basePlugin->name()) {
            qWarning() << "Plugin name mismatch! Expected:" << pluginName << "Actual:" << basePlugin->name();
        }

        qDebug() << "Plugin name:" << basePlugin->name();
        qDebug() << "Plugin version:" << basePlugin->version();

        // Initialize LogosAPI for this plugin
        LogosAPI* logos_api = new LogosAPI(pluginName, plugin);
        qDebug() << "LogosAPI initialized for plugin (Local mode):" << pluginName;

        // Register the plugin for access using LogosAPI Provider
        // In Local mode, this uses PluginRegistry instead of QRemoteObjects
        bool success = logos_api->getProvider()->registerObject(basePlugin->name(), plugin);
        if (!success) {
            qCritical() << "Failed to register plugin (Local mode):" << basePlugin->name();
            delete logos_api;
            return false;
        }

        qDebug() << "Plugin registered with PluginRegistry (Local mode):" << basePlugin->name();

        // Generate and save auth token
        QUuid authToken = QUuid::createUuid();
        QString authTokenString = authToken.toString(QUuid::WithoutBraces);
        qDebug() << "Generated auth token (Local mode):" << authTokenString;

        // Save auth tokens for core access
        logos_api->getTokenManager()->saveToken("core", authTokenString);
        logos_api->getTokenManager()->saveToken("core_manager", authTokenString);
        logos_api->getTokenManager()->saveToken("capability_module", authTokenString);
        qDebug() << "Auth tokens saved for core access (Local mode)";

        // Also save the plugin token in the global TokenManager
        TokenManager& tokenManager = TokenManager::instance();
        tokenManager.saveToken(pluginName, authTokenString);

        // Store the LogosAPI instance for cleanup
        g_local_plugin_apis.insert(pluginName, logos_api);

        // Add the plugin name to our loaded plugins list
        g_loaded_plugins.append(pluginName);

        qDebug() << "Plugin" << pluginName << "is now running in-process (Local mode)";
        return true;
    }

#ifdef Q_OS_IOS
    // iOS doesn't support spawning child processes
    qWarning() << "Plugin loading via separate processes not supported on iOS:" << pluginName;
    qWarning() << "Consider using Local mode with LogosModeConfig::setMode(LogosMode::Local)";
    return false;
#else
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
#ifdef Q_OS_WIN
    if (!logosHostPath.endsWith(".exe")) {
        logosHostPath += ".exe";
    }
#endif

    // 3) Fallback relative to plugins directory (../../logos-liblogos/build/bin/logos_host)
    if (!QFile::exists(logosHostPath)) {
        if (!g_plugins_dirs.isEmpty()) {
            QDir pluginsDirCandidate(g_plugins_dirs.first());
            QString candidate = QDir::cleanPath(pluginsDirCandidate.absoluteFilePath("../bin/logos_host"));
#ifdef Q_OS_WIN
            if (!candidate.endsWith(".exe")) candidate += ".exe";
#endif
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

        // Create LogosAPI instance to connect to the capability module
        LogosAPI* coreAPI = new LogosAPI("core");

        // Use a timer to ensure the capability module is ready
        QTimer* informTimer = new QTimer();
        informTimer->setSingleShot(true);
        informTimer->setInterval(1000); // 1 second delay to ensure connection is ready

        // get token for capability_module
        QString capabilityModuleToken = tokenManager.getToken("capability_module");
        qDebug() << "Capability module token:" << capabilityModuleToken;

        QObject::connect(informTimer, &QTimer::timeout, [=]() {
            if (coreAPI->getClient("capability_module")->isConnected()) {
                qDebug() << "Calling informModuleToken on capability module";
                
                // Call informModuleToken with the core auth token, module name, and module token
                bool success = coreAPI->getClient("capability_module")->informModuleToken(capabilityModuleToken, pluginName, authTokenString);
                if (success) {
                    qDebug() << "Successfully informed capability module about token for:" << pluginName;
                } else {
                    qWarning() << "Failed to inform capability module about token for:" << pluginName;
                }
            } else {
                qWarning() << "Failed to connect to capability module for token notification";
            }
            
            // Clean up
            coreAPI->deleteLater();
            informTimer->deleteLater();
        });
        
        informTimer->start();
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
#endif // Q_OS_IOS
}

void loadAndProcessPlugin(const QString &pluginPath)
{
    // First process the plugin to get its metadata
    QString pluginName = processPlugin(pluginPath);
    
    // If we found the name, load the plugin
    if (!pluginName.isEmpty()) {
        loadPlugin(pluginName);
    } else {
        qWarning() << "Failed to process plugin:" << pluginPath;
    }
}

QStringList findPlugins(const QString &pluginsDir)
{
    QDir dir(pluginsDir);
    QStringList plugins;
    
    qDebug() << "Searching for plugins in:" << dir.absolutePath();
    
    if (!dir.exists()) {
        qWarning() << "Plugins directory does not exist:" << dir.absolutePath();
        return plugins;
    }
    
    // Get all files in the directory
    QStringList entries = dir.entryList(QDir::Files);
    qDebug() << "Files found:" << entries;
    
    // Filter for plugin files based on platform
    QStringList nameFilters;
#ifdef Q_OS_WIN
    nameFilters << "*.dll";
#elif defined(Q_OS_MAC)
    nameFilters << "*.dylib";
#else
    nameFilters << "*.so";
#endif
    
    dir.setNameFilters(nameFilters);
    QStringList pluginFiles = dir.entryList(QDir::Files);
    
    for (const QString &fileName : pluginFiles) {
        QString filePath = dir.absoluteFilePath(fileName);
        plugins.append(filePath);
        qDebug() << "Found plugin:" << filePath;
    }
    
    return plugins;
}

bool initializeCoreManager()
{
    qDebug() << "\n=== Initializing Core Manager ===";
    
    // Create the core manager instance directly
    CoreManagerPlugin* coreManager = new CoreManagerPlugin();
    
    // Create LogosAPI instance for core manager registration
    LogosAPI* coreAPI = new LogosAPI("core_manager");
    
    // Register the core manager using the new API (which will wrap it with ModuleProxy)
    bool success = coreAPI->getProvider()->registerObject(coreManager->name(), coreManager);
    if (success) {
        qDebug() << "Core manager registered using new API with name:" << coreManager->name();
        // generate token
        QUuid coreManagerToken = QUuid::createUuid();
        QString coreManagerTokenString = coreManagerToken.toString(QUuid::WithoutBraces);
        qDebug() << "Generated core manager token:" << coreManagerTokenString;
        // TODO: replace this, using test token
        coreAPI->getTokenManager()->saveToken("core_manager", coreManagerTokenString);
        qDebug() << "Test token saved for core access";
    } else {
        qWarning() << "Failed to register core manager using new API";
        delete coreAPI;
        return false;
    }
    
    // Add to loaded plugins list
    g_loaded_plugins.append(coreManager->name());
    
    qDebug() << "Core manager initialized successfully";
    return true;
}

bool initializeCapabilityModule()
{
    qDebug() << "\n=== Initializing Capability Module ===";

    // Check if capability_module is available in known plugins
    if (!g_known_plugins.contains("capability_module")) {
        qDebug() << "Capability module not found in known plugins, skipping initialization";
        return false;
    }

    qDebug() << "Capability module found, attempting to load...";

    // Load the capability module
    bool success = loadPlugin("capability_module");
    if (success) {
        qDebug() << "Capability module loaded successfully";
        
        // Inform capability module about core_manager token
        if (g_loaded_plugins.contains("core_manager")) {
            qDebug() << "Informing capability module about core_manager token";
            
            // Get the core_manager token from TokenManager
            TokenManager& tokenManager = TokenManager::instance();
            QString coreManagerToken = tokenManager.getToken("core_manager");
            
            if (!coreManagerToken.isEmpty()) {
                // Create LogosAPI instance to connect to the capability module
                LogosAPI* coreAPI = new LogosAPI("core");
                
                // Use a timer to ensure the capability module is ready
                QTimer* informTimer = new QTimer();
                informTimer->setSingleShot(true);
                informTimer->setInterval(1000); // 1 second delay to ensure connection is ready
                
                // get token for capability_module
                QString capabilityModuleToken = tokenManager.getToken("capability_module");
                qDebug() << "Capability module token:" << capabilityModuleToken;

                // get token for core_manager
                QString coreManagerToken = tokenManager.getToken("core_manager");
                qDebug() << "Core manager token:" << coreManagerToken;
                
                QObject::connect(informTimer, &QTimer::timeout, [=]() {
                    if (coreAPI->getClient("capability_module")->isConnected()) {
                        qDebug() << "Calling informModuleToken on capability module for core_manager";
                        
                        // Call informModuleToken with the core auth token, module name, and module token
                        bool success = coreAPI->getClient("capability_module")->informModuleToken(capabilityModuleToken, "core_manager", coreManagerToken);
                        if (success) {
                            qDebug() << "Successfully informed capability module about core_manager token";
                        } else {
                            qWarning() << "Failed to inform capability module about core_manager token";
                        }
                    } else {
                        qWarning() << "Failed to connect to capability module for core_manager token notification";
                    }
                    
                    // Clean up
                    coreAPI->deleteLater();
                    informTimer->deleteLater();
                });
                
                informTimer->start();
            } else {
                qWarning() << "No token found for core_manager, skipping capability module notification";
            }
        } else {
            qDebug() << "Core manager not loaded, skipping token notification";
        }
        
        return true;
    } else {
        qWarning() << "Failed to load capability module";
        return false;
    }
}

int loadStaticPlugins()
{
    if (!LogosModeConfig::isLocal()) {
        qWarning() << "loadStaticPlugins() requires Local mode.";
        return 0;
    }

    int loadedCount = 0;
    
    // Get all statically registered plugin instances
    // These are registered via Q_IMPORT_PLUGIN() in the application
    const QObjectList staticPlugins = QPluginLoader::staticInstances();
    
    qDebug() << "Found" << staticPlugins.size() << "static plugin instances";
    
    for (QObject* pluginObject : staticPlugins) {
        if (!pluginObject) {
            qWarning() << "Null static plugin instance, skipping";
            continue;
        }
        
        // Cast to PluginInterface
        PluginInterface* basePlugin = qobject_cast<PluginInterface*>(pluginObject);
        if (!basePlugin) {
            qDebug() << "Static plugin" << pluginObject->metaObject()->className() 
                     << "does not implement PluginInterface, skipping";
            continue;
        }
        
        QString pluginName = basePlugin->name();
        
        // Skip core_manager as it's handled separately
        if (pluginName == "core_manager") {
            qDebug() << "Skipping core_manager (already loaded)";
            continue;
        }
        
        // Check if already loaded
        if (g_loaded_plugins.contains(pluginName)) {
            qDebug() << "Static plugin already loaded:" << pluginName;
            continue;
        }
        
        qDebug() << "Loading static plugin:" << pluginName << "version:" << basePlugin->version();
        
        // Initialize LogosAPI for this plugin
        LogosAPI* logos_api = new LogosAPI(pluginName, pluginObject);
        
        // Register the plugin with the provider
        bool success = logos_api->getProvider()->registerObject(pluginName, pluginObject);
        if (success) {
            qDebug() << "Static plugin registered:" << pluginName;
            
            // Generate and save auth token
            QUuid authToken = QUuid::createUuid();
            QString authTokenString = authToken.toString(QUuid::WithoutBraces);
            
            logos_api->getTokenManager()->saveToken("core", authTokenString);
            logos_api->getTokenManager()->saveToken("core_manager", authTokenString);
            logos_api->getTokenManager()->saveToken("capability_module", authTokenString);
            
            // Save in global TokenManager
            TokenManager& tokenManager = TokenManager::instance();
            tokenManager.saveToken(pluginName, authTokenString);
            
            // Store for cleanup
            g_local_plugin_apis.insert(pluginName, logos_api);
            
            // Add to loaded plugins list
            g_loaded_plugins.append(pluginName);
            
            // Add to known plugins (with empty path since it's static)
            g_known_plugins.insert(pluginName, QString("static:%1").arg(pluginName));
            
            loadedCount++;
            qDebug() << "Static plugin" << pluginName << "loaded successfully";
        } else {
            qCritical() << "Failed to register static plugin:" << pluginName;
            delete logos_api;
        }
    }
    
    qDebug() << "Loaded" << loadedCount << "static plugins";
    return loadedCount;
}

bool registerPluginInstance(const QString& pluginName, void* plugin_instance)
{
    if (!plugin_instance) {
        qWarning() << "registerPluginInstance: Invalid arguments (instance is null)";
        return false;
    }
    
    if (!LogosModeConfig::isLocal()) {
        qWarning() << "registerPluginInstance() requires Local mode.";
        return false;
    }
    
    QObject* pluginObject = static_cast<QObject*>(plugin_instance);
    
    qDebug() << "registerPluginInstance: Registering plugin:" << pluginName;
    
    PluginInterface* basePlugin = qobject_cast<PluginInterface*>(pluginObject);
    if (!basePlugin) {
        qCritical() << "Plugin" << pluginName << "does not implement PluginInterface";
        return false;
    }
    
    QString actualName = basePlugin->name();
    QString nameToUse = pluginName;
    if (actualName != pluginName) {
        qWarning() << "Plugin name mismatch: expected" << pluginName << "but got" << actualName;
        // Use the actual name from the plugin
        nameToUse = actualName;
    }
    
    // Check if already loaded
    if (g_loaded_plugins.contains(nameToUse)) {
        qDebug() << "Plugin already registered:" << nameToUse;
        return true; // Already registered, consider success
    }
    
    qDebug() << "Registering plugin:" << nameToUse << "version:" << basePlugin->version();
    
    // Initialize LogosAPI for this plugin
    LogosAPI* logos_api = new LogosAPI(nameToUse, pluginObject);
    
    // Register the plugin with the provider
    bool success = logos_api->getProvider()->registerObject(nameToUse, pluginObject);
    if (!success) {
        qCritical() << "Failed to register plugin with provider:" << nameToUse;
        delete logos_api;
        return false;
    }
    qDebug() << "Plugin registered with provider:" << nameToUse;

    // Generate and save auth token
    QUuid authToken = QUuid::createUuid();
    QString authTokenString = authToken.toString(QUuid::WithoutBraces);

    logos_api->getTokenManager()->saveToken("core", authTokenString);
    logos_api->getTokenManager()->saveToken("core_manager", authTokenString);
    logos_api->getTokenManager()->saveToken("capability_module", authTokenString);
    logos_api->getTokenManager()->saveToken("package_manager", authTokenString);

    // Save in global TokenManager
    TokenManager& tokenManager = TokenManager::instance();
    tokenManager.saveToken(nameToUse, authTokenString);

    g_local_plugin_apis.insert(nameToUse, logos_api);
    g_loaded_plugins.append(nameToUse);
    g_known_plugins.insert(nameToUse, QString("app:%1").arg(nameToUse));

    qDebug() << "Plugin" << nameToUse << "registered successfully";
    return true;
}

bool registerPluginByName(const QString& pluginName)
{
    if (!LogosModeConfig::isLocal()) {
        qWarning() << "registerPluginByName() requires Local mode.";
        return false;
    }
    
    qDebug() << "registerPluginByName: Looking for plugin:" << pluginName;
    
    const QObjectList staticPlugins = QPluginLoader::staticInstances();
    qDebug() << "Found" << staticPlugins.size() << "static plugin instances";
    
    for (QObject* obj : staticPlugins) {
        if (!obj) continue;
        
        PluginInterface* plugin = qobject_cast<PluginInterface*>(obj);
        if (plugin && plugin->name() == pluginName) {
            qDebug() << "Found matching static plugin:" << pluginName;
            return registerPluginInstance(pluginName, obj);
        }
    }
    
    qWarning() << "Static plugin not found:" << pluginName;
    qWarning() << "Available static plugins:";
    for (QObject* obj : staticPlugins) {
        if (!obj) continue;
        PluginInterface* plugin = qobject_cast<PluginInterface*>(obj);
        if (plugin) {
            qWarning() << "  -" << plugin->name();
        } else {
            qWarning() << "  - (non-PluginInterface)" << obj->metaObject()->className();
        }
    }
    
    return false;
}

bool unloadPlugin(const QString& pluginName)
{
#ifdef Q_OS_IOS
    // iOS doesn't support process-based plugins
    qWarning() << "Plugin unloading not supported on iOS";
    return false;
#else
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
#endif // Q_OS_IOS
}

char** getLoadedPluginsCStr()
{
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

char** getKnownPluginsCStr()
{
    qDebug() << "getKnownPluginsCStr() called";
    qDebug() << "g_known_plugins size:" << g_known_plugins.size();
    qDebug() << "g_known_plugins keys:" << g_known_plugins.keys();
    
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
    
    qDebug() << "Returning" << count << "known plugins";
    
    // Allocate memory for the array of strings
    char** result = new char*[count + 1];  // +1 for null terminator
    
    // Copy each plugin name
    for (int i = 0; i < count; ++i) {
        QByteArray utf8Data = knownPlugins[i].toUtf8();
        result[i] = new char[utf8Data.size() + 1];
        strcpy(result[i], utf8Data.constData());
        qDebug() << "  -" << knownPlugins[i];
    }
    
    // Null-terminate the array
    result[count] = nullptr;
    
    return result;
}

QStringList getLoadedPlugins()
{
    return g_loaded_plugins;
}

QHash<QString, QString> getKnownPlugins()
{
    return g_known_plugins;
}

bool isPluginLoaded(const QString& name)
{
    return g_loaded_plugins.contains(name);
}

bool isPluginKnown(const QString& name)
{
    return g_known_plugins.contains(name);
}

} // namespace PluginManager
