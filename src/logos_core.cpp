#include "logos_core.h"
#include <QCoreApplication>
#include <QPluginLoader>
#include <QObject>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMetaProperty>
#include <QMetaMethod>
#include <QTimer>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QHash>
#include <QRemoteObjectRegistryHost>
// QProcess is not available on iOS
#ifndef Q_OS_IOS
#include <QProcess>
#include <QLocalSocket>
#endif
#include <QLocalServer>
#include <QUuid>
#include <QThread>
#include <QDateTime>
#include <QPair>
#include <cmath>
#include <cstring>
#include "../interface.h"
#include "core_manager/core_manager.h"
#include "logos_api.h"
#include "logos_api_provider.h"
#include "logos_api_client.h"
#include "token_manager.h"
#include "logos_mode.h"

// Platform-specific includes for process monitoring
#if (defined(Q_OS_MACOS) || defined(Q_OS_MAC)) && !defined(Q_OS_IOS)
#include <libproc.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#include <sys/sysctl.h>
#elif defined(Q_OS_LINUX)
#include <sys/resource.h>
#include <sys/times.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#endif

// Declare QObject* as a metatype so it can be stored in QVariant
Q_DECLARE_METATYPE(QObject*)

// Global application pointer
static QCoreApplication* g_app = nullptr;

// Flag to track if we created the app or are using an existing one
static bool g_app_created_by_us = false;

// Custom plugins directory
static QString g_plugins_dir = "";

// Global list to store loaded plugin names
static QStringList g_loaded_plugins;

// Global hash to store known plugin names and paths
static QHash<QString, QString> g_known_plugins;

// Global hash to store plugin processes
#ifndef Q_OS_IOS
static QHash<QString, QProcess*> g_plugin_processes;
#endif

// Global hash to store LogosAPI instances for Local mode plugins
static QHash<QString, LogosAPI*> g_local_plugin_apis;

// Global Qt Remote Object registry host
static QRemoteObjectRegistryHost* g_registry_host = nullptr;

// Structure to store event listener information
struct EventListener {
    QString pluginName;
    QString eventName;
    AsyncCallback callback;
    void* userData;
};

// Global list to store registered event listeners
static QList<EventListener> g_event_listeners;

struct ProcessStats {
    double cpuPercent;
    double cpuTimeSeconds;
    double memoryMB;
};

static QHash<qint64, QPair<double, qint64>> g_previous_cpu_times;

// Helper function to process a plugin and extract its metadata
static QString processPlugin(const QString &pluginPath)
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

// Helper function to load a plugin by name
static bool loadPlugin(const QString &pluginName)
{
    if (!g_known_plugins.contains(pluginName)) {
        qWarning() << "Cannot load unknown plugin:" << pluginName;
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
        if (!g_plugins_dir.isEmpty()) {
            QDir pluginsDirCandidate(g_plugins_dir);
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

    //

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

// Helper function to load and process a plugin
static void loadAndProcessPlugin(const QString &pluginPath)
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

// Helper function to find and load all plugins in a directory
static QStringList findPlugins(const QString &pluginsDir)
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

static bool initializeCapabilityModule()
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

// Helper function to initialize core manager
static bool initializeCoreManager()
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

void logos_core_init(int argc, char *argv[])
{
    // Check if an application instance already exists (e.g., when used in a Qt Quick app)
    if (QCoreApplication::instance()) {
        // Use the existing application instance
        g_app = QCoreApplication::instance();
        g_app_created_by_us = false;
        qDebug() << "Using existing QCoreApplication instance";
    } else {
        // Create a new application instance (standalone mode)
        g_app = new QCoreApplication(argc, argv);
        g_app_created_by_us = true;
        qDebug() << "Created new QCoreApplication instance";
    }
    
    // Register QObject* as a metatype
    qRegisterMetaType<QObject*>("QObject*");
}

void logos_core_set_mode(int mode)
{
    if (mode == 1) {
        LogosModeConfig::setMode(LogosMode::Local);
        qDebug() << "Logos mode set to: Local (in-process)";
    } else {
        LogosModeConfig::setMode(LogosMode::Remote);
        qDebug() << "Logos mode set to: Remote (separate processes)";
    }
}

void logos_core_set_plugins_dir(const char* plugins_dir)
{
    if (plugins_dir) {
        g_plugins_dir = QString(plugins_dir);
        qDebug() << "Custom plugins directory set to:" << g_plugins_dir;
    }
}

void logos_core_start()
{
    qDebug() << "Simple Plugin Example";
    qDebug() << "Current directory:" << QDir::currentPath();
    
    // Clear the list of loaded plugins before loading new ones
    g_loaded_plugins.clear();
    
    // Initialize Qt Remote Object registry host
    if (!g_registry_host) {
        g_registry_host = new QRemoteObjectRegistryHost(QUrl(QStringLiteral("local:logos_core_manager")));
        qDebug() << "Qt Remote Object registry host initialized at: local:logos_core_manager";
    }
    
    // First initialize the core manager
    if (!initializeCoreManager()) {
        qWarning() << "Failed to initialize core manager, continuing with other modules...";
    }
    
    // Define the plugins directory path
    QString pluginsDir;
    if (!g_plugins_dir.isEmpty()) {
        // Use the custom plugins directory if set
        pluginsDir = g_plugins_dir;
    } else {
        // Use the default plugins directory
        pluginsDir = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../modules");
    }
    qDebug() << "Looking for modules in:" << pluginsDir;
    
    // Find and process all plugins in the directory to populate g_known_plugins
    QStringList pluginPaths = findPlugins(pluginsDir);
    
    if (pluginPaths.isEmpty()) {
        qWarning() << "No modules found in:" << pluginsDir;
    } else {
        qDebug() << "Found" << pluginPaths.size() << "modules";
        
        // Process each plugin to add to known plugins list
        for (const QString &pluginPath : pluginPaths) {
            QString pluginName = processPlugin(pluginPath);
            if (pluginName.isEmpty()) {
                qWarning() << "Failed to process plugin (no metadata or invalid):" << pluginPath;
            } else {
                qDebug() << "Successfully processed plugin:" << pluginName;
            }
        }
        
        qDebug() << "Total known plugins after processing:" << g_known_plugins.size();
        qDebug() << "Known plugin names:" << g_known_plugins.keys();

        // Initialize capability module if available (after plugin discovery)
        initializeCapabilityModule();
    }
}

int logos_core_exec()
{
    if (g_app) {
        return g_app->exec();
    }
    return -1;
}

void logos_core_cleanup()
{
    // Clean up Local mode plugins
    if (!g_local_plugin_apis.isEmpty()) {
        qDebug() << "Cleaning up Local mode plugins...";
        for (auto it = g_local_plugin_apis.begin(); it != g_local_plugin_apis.end(); ++it) {
            QString pluginName = it.key();
            LogosAPI* logos_api = it.value();
            
            qDebug() << "Cleaning up Local mode plugin:" << pluginName;
            delete logos_api;
        }
        g_local_plugin_apis.clear();
        qDebug() << "Local mode plugins cleaned up";
    }

#ifndef Q_OS_IOS
    // Terminate all plugin processes (Remote mode)
    if (!g_plugin_processes.isEmpty()) {
        qDebug() << "Terminating all plugin processes...";
        for (auto it = g_plugin_processes.begin(); it != g_plugin_processes.end(); ++it) {
            QProcess* process = it.value();
            QString pluginName = it.key();
            
            qDebug() << "Terminating plugin process:" << pluginName;
            process->terminate();
            
            if (!process->waitForFinished(3000)) {
                qWarning() << "Process did not terminate gracefully, killing it:" << pluginName;
                process->kill();
                process->waitForFinished(1000);
            }
            
            delete process;
        }
        g_plugin_processes.clear();
    }
#endif
    g_loaded_plugins.clear();
    
    // Clean up Qt Remote Object registry host
    if (g_registry_host) {
        delete g_registry_host;
        g_registry_host = nullptr;
        qDebug() << "Qt Remote Object registry host cleaned up";
    }
    
    // Only delete the app if we created it
    if (g_app_created_by_us) {
        delete g_app;
    }
    g_app = nullptr;
    g_app_created_by_us = false;
}

// Implementation of the function to get loaded plugins
char** logos_core_get_loaded_plugins()
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

// Implementation of the function to get known plugins
char** logos_core_get_known_plugins()
{
    qDebug() << "logos_core_get_known_plugins() called";
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

// Implementation of the function to load a plugin by name
int logos_core_load_plugin(const char* plugin_name)
{
    if (!plugin_name) {
        qWarning() << "Cannot load plugin: name is null";
        return 0;
    }
    
    QString name = QString::fromUtf8(plugin_name);
    qDebug() << "Attempting to load plugin by name:" << name;
    
    // Check if plugin exists in known plugins
    if (!g_known_plugins.contains(name)) {
        qWarning() << "Plugin not found among known plugins:" << name;
        return 0;
    }
    
    // Use our internal loadPlugin function
    bool success = loadPlugin(name);
    return success ? 1 : 0;
}

int logos_core_load_static_plugins()
{
    if (!LogosModeConfig::isLocal()) {
        qWarning() << "logos_core_load_static_plugins() requires Local mode. Call logos_core_set_mode(LOGOS_MODE_LOCAL) first.";
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

int logos_core_register_plugin_instance(const char* plugin_name, void* plugin_instance)
{
    if (!plugin_name || !plugin_instance) {
        qWarning() << "logos_core_register_plugin_instance: Invalid arguments (name or instance is null)";
        return 0;
    }
    
    if (!LogosModeConfig::isLocal()) {
        qWarning() << "logos_core_register_plugin_instance() requires Local mode. Call logos_core_set_mode(LOGOS_MODE_LOCAL) first.";
        return 0;
    }
    
    QString pluginName = QString::fromUtf8(plugin_name);
    QObject* pluginObject = static_cast<QObject*>(plugin_instance);
    
    qDebug() << "logos_core_register_plugin_instance: Registering plugin:" << pluginName;
    
    PluginInterface* basePlugin = qobject_cast<PluginInterface*>(pluginObject);
    if (!basePlugin) {
        qCritical() << "Plugin" << pluginName << "does not implement PluginInterface";
        return 0;
    }
    
    QString actualName = basePlugin->name();
    if (actualName != pluginName) {
        qWarning() << "Plugin name mismatch: expected" << pluginName << "but got" << actualName;
        // Use the actual name from the plugin
        pluginName = actualName;
    }
    
    // Check if already loaded
    if (g_loaded_plugins.contains(pluginName)) {
        qDebug() << "Plugin already registered:" << pluginName;
        return 1; // Already registered, consider success
    }
    
    qDebug() << "Registering plugin:" << pluginName << "version:" << basePlugin->version();
    
    // Initialize LogosAPI for this plugin
    LogosAPI* logos_api = new LogosAPI(pluginName, pluginObject);
    
    // Register the plugin with the provider
    bool success = logos_api->getProvider()->registerObject(pluginName, pluginObject);
    if (!success) {
        qCritical() << "Failed to register plugin with provider:" << pluginName;
        delete logos_api;
        return 0;
    }

    qDebug() << "Plugin registered with provider:" << pluginName;

    // Generate and save auth token
    QUuid authToken = QUuid::createUuid();
    QString authTokenString = authToken.toString(QUuid::WithoutBraces);

    logos_api->getTokenManager()->saveToken("core", authTokenString);
    logos_api->getTokenManager()->saveToken("core_manager", authTokenString);
    logos_api->getTokenManager()->saveToken("capability_module", authTokenString);
    logos_api->getTokenManager()->saveToken("package_manager", authTokenString);

    // Save in global TokenManager
    TokenManager& tokenManager = TokenManager::instance();
    tokenManager.saveToken(pluginName, authTokenString);

    g_local_plugin_apis.insert(pluginName, logos_api);
    g_loaded_plugins.append(pluginName);
    g_known_plugins.insert(pluginName, QString("app:%1").arg(pluginName));

    qDebug() << "Plugin" << pluginName << "registered successfully";
    return 1;
}

// Implementation of the function to unload a plugin by name
int logos_core_unload_plugin(const char* plugin_name)
{
#ifdef Q_OS_IOS
    // iOS doesn't support process-based plugins
    qWarning() << "Plugin unloading not supported on iOS";
    Q_UNUSED(plugin_name);
    return 0;
#else
    if (!plugin_name) {
        qWarning() << "Cannot unload plugin: name is null";
        return 0;
    }

    QString name = QString::fromUtf8(plugin_name);
    qDebug() << "Attempting to unload plugin by name:" << name;

    // Check if plugin is loaded
    if (!g_loaded_plugins.contains(name)) {
        qWarning() << "Plugin not loaded, cannot unload:" << name;
        qDebug() << "Loaded plugins:" << g_loaded_plugins;
        return 0;
    }

    // Check if we have a process for this plugin
    if (!g_plugin_processes.contains(name)) {
        qWarning() << "No process found for plugin:" << name;
        return 0;
    }

    // Get the process
    QProcess* process = g_plugin_processes.value(name);
    
    qDebug() << "Terminating plugin process for:" << name;
    
    // Terminate the process gracefully
    process->terminate();
    
    // Wait for the process to finish, with a timeout
    if (!process->waitForFinished(5000)) {
        qWarning() << "Process did not terminate gracefully, killing it";
        process->kill();
        process->waitForFinished(2000);
    }

    // Remove from our tracking structures
    g_plugin_processes.remove(name);
    g_loaded_plugins.removeAll(name);
    
    // The process will be cleaned up by the signal handler
    qDebug() << "Successfully unloaded plugin:" << name;
    return 1;
#endif // Q_OS_IOS
}

// TODO: this function can probably go to the core manager instead
char* logos_core_process_plugin(const char* plugin_path)
{
    if (!plugin_path) {
        qWarning() << "Cannot process plugin: path is null";
        return nullptr;
    }

    QString path = QString::fromUtf8(plugin_path);
    qDebug() << "Processing plugin file:" << path;

    QString pluginName = processPlugin(path);
    if (pluginName.isEmpty()) {
        qWarning() << "Failed to process plugin file:" << path;
        return nullptr;
    }

    // Convert to C string that must be freed by the caller
    QByteArray utf8Data = pluginName.toUtf8();
    char* result = new char[utf8Data.size() + 1];
    strcpy(result, utf8Data.constData());

    return result;
}

// Implementation of the function to get a token by key
char* logos_core_get_token(const char* key)
{
    if (!key) {
        qWarning() << "Cannot get token: key is null";
        return nullptr;
    }

    QString keyStr = QString::fromUtf8(key);
    qDebug() << "Getting token for key:" << keyStr;

    // Get the token from the TokenManager singleton
    TokenManager& tokenManager = TokenManager::instance();
    QString token = tokenManager.getToken(keyStr);

    if (token.isEmpty()) {
        qDebug() << "No token found for key:" << keyStr;
        return nullptr;
    }

    qDebug() << "Token found for key:" << keyStr;

    // Convert to C string that must be freed by the caller
    QByteArray utf8Data = token.toUtf8();
    char* result = new char[utf8Data.size() + 1];
    strcpy(result, utf8Data.constData());

    return result;
}

// === Async Callback API Implementation ===

void logos_core_async_operation(const char* data, AsyncCallback callback, void* user_data)
{
    if (!callback) {
        qWarning() << "logos_core_async_operation: callback is null";
        return;
    }
    
    QString inputData = data ? QString::fromUtf8(data) : QString("no data");
    qDebug() << "Starting async operation with data:" << inputData;
    
    // Create a timer to simulate async work
    QTimer* timer = new QTimer();
    timer->setSingleShot(true);
    timer->setInterval(2000); // 2 second delay
    
    // Connect the timer to execute the callback
    QObject::connect(timer, &QTimer::timeout, [=]() {
        qDebug() << "Async operation completed for data:" << inputData;
        
        // Create the result message
        QString resultMessage = QString("Async operation completed successfully for: %1").arg(inputData);
        QByteArray messageBytes = resultMessage.toUtf8();
        
        // Call the callback with success (1) and message
        callback(1, messageBytes.constData(), user_data);
        
        // Clean up the timer
        timer->deleteLater();
    });
    
    timer->start();
    qDebug() << "Async operation timer started, will complete in 2 seconds";
}

void logos_core_load_plugin_async(const char* plugin_name, AsyncCallback callback, void* user_data)
{
    if (!callback) {
        qWarning() << "logos_core_load_plugin_async: callback is null";
        return;
    }
    
    if (!plugin_name) {
        qWarning() << "logos_core_load_plugin_async: plugin_name is null";
        callback(0, "Plugin name is null", user_data);
        return;
    }
    
    QString name = QString::fromUtf8(plugin_name);
    qDebug() << "Starting async plugin load for:" << name;
    
    // Check if plugin exists in known plugins
    if (!g_known_plugins.contains(name)) {
        QString errorMsg = QString("Plugin not found among known plugins: %1").arg(name);
        QByteArray errorBytes = errorMsg.toUtf8();
        callback(0, errorBytes.constData(), user_data);
        return;
    }
    
    // Create a timer to simulate async plugin loading
    QTimer* timer = new QTimer();
    timer->setSingleShot(true);
    timer->setInterval(1000); // 1 second delay to simulate work
    
    // Connect the timer to execute the actual plugin loading
    QObject::connect(timer, &QTimer::timeout, [=]() {
        qDebug() << "Executing async plugin load for:" << name;
        
        // Use our existing synchronous loadPlugin function
        bool success = loadPlugin(name);
        
        QString resultMessage;
        if (success) {
            resultMessage = QString("Plugin '%1' loaded successfully").arg(name);
        } else {
            resultMessage = QString("Failed to load plugin '%1'").arg(name);
        }
        
        QByteArray messageBytes = resultMessage.toUtf8();
        
        // Call the callback with the result
        callback(success ? 1 : 0, messageBytes.constData(), user_data);
        
        // Clean up the timer
        timer->deleteLater();
    });
    
    timer->start();
    qDebug() << "Async plugin load timer started for:" << name;
}

// Helper function to convert JSON parameter to QVariant
static QVariant jsonParamToQVariant(const QJsonObject& param) {
    QString name = param.value("name").toString();
    QString value = param.value("value").toString();
    QString type = param.value("type").toString();
    
    qDebug() << "Converting param:" << name << "value:" << value << "type:" << type;
    
    if (type == "string" || type == "QString") {
        return QVariant(value);
    } else if (type == "int" || type == "integer") {
        bool ok;
        int intValue = value.toInt(&ok);
        return ok ? QVariant(intValue) : QVariant();
    } else if (type == "bool" || type == "boolean") {
        if (value.toLower() == "true" || value == "1") {
            return QVariant(true);
        } else if (value.toLower() == "false" || value == "0") {
            return QVariant(false);
        }
        return QVariant();
    } else if (type == "double" || type == "float") {
        bool ok;
        double doubleValue = value.toDouble(&ok);
        return ok ? QVariant(doubleValue) : QVariant();
    } else {
        // Default to string for unknown types
        qWarning() << "Unknown parameter type:" << type << "- treating as string";
        return QVariant(value);
    }
}

void logos_core_call_plugin_method_async(
    const char* plugin_name, 
    const char* method_name, 
    const char* params_json, 
    AsyncCallback callback, 
    void* user_data
) {
    if (!callback) {
        qWarning() << "logos_core_call_plugin_method_async: callback is null";
        return;
    }
    
    if (!plugin_name || !method_name) {
        qWarning() << "logos_core_call_plugin_method_async: plugin_name or method_name is null";
        callback(0, "Plugin name or method name is null", user_data);
        return;
    }
    
    QString pluginNameStr = QString::fromUtf8(plugin_name);
    QString methodNameStr = QString::fromUtf8(method_name);
    QString paramsJsonStr = params_json ? QString::fromUtf8(params_json) : QString("[]");
    
    qDebug() << "Starting async method call for plugin:" << pluginNameStr 
             << "method:" << methodNameStr 
             << "params:" << paramsJsonStr;
    
    // Check if plugin is loaded
    if (!g_loaded_plugins.contains(pluginNameStr)) {
        QString errorMsg = QString("Plugin not loaded: %1").arg(pluginNameStr);
        QByteArray errorBytes = errorMsg.toUtf8();
        callback(0, errorBytes.constData(), user_data);
        return;
    }
    
    // Create a timer to simulate async method call
    // Create timer with QCoreApplication as parent to ensure it's in the main thread
    QTimer* timer = new QTimer(QCoreApplication::instance());
    timer->setSingleShot(true);
    timer->setInterval(500); // 0.5 second delay to simulate async work
        
        // Connect the timer to execute the actual method call
        QObject::connect(timer, &QTimer::timeout, [=]() {
        qDebug() << "Executing async method call for:" << pluginNameStr << "::" << methodNameStr;
        
        try {
            // Parse the JSON parameters
            QJsonParseError parseError;
            QJsonDocument jsonDoc = QJsonDocument::fromJson(paramsJsonStr.toUtf8(), &parseError);
            
            if (parseError.error != QJsonParseError::NoError) {
                QString errorMsg = QString("JSON parse error: %1").arg(parseError.errorString());
                QByteArray errorBytes = errorMsg.toUtf8();
                callback(0, errorBytes.constData(), user_data);
                timer->deleteLater();
                return;
            }
            
            QJsonArray paramsArray = jsonDoc.array();
            QVariantList args;
            
            // Convert JSON parameters to QVariantList
            for (const QJsonValue& paramValue : paramsArray) {
                if (paramValue.isObject()) {
                    QJsonObject paramObj = paramValue.toObject();
                    QVariant variant = jsonParamToQVariant(paramObj);
                    if (variant.isValid()) {
                        args.append(variant);
                    } else {
                        QString errorMsg = QString("Invalid parameter: %1").arg(paramObj.value("name").toString());
                        QByteArray errorBytes = errorMsg.toUtf8();
                        callback(0, errorBytes.constData(), user_data);
                        timer->deleteLater();
                        return;
                    }
                }
            }
            
            qDebug() << "Converted parameters to QVariantList, count:" << args.size();
            
            // Create LogosAPI instance to make the remote call
            LogosAPI* logosAPI = new LogosAPI("core");
            
            // Use a longer delay to ensure connection is established
            QTimer* connectionTimer = new QTimer();
            connectionTimer->setSingleShot(true);
            connectionTimer->setInterval(2000); // 2 second delay
            
            QObject::connect(connectionTimer, &QTimer::timeout, [=]() {
                if (logosAPI->getClient(pluginNameStr)->isConnected()) {
                    qDebug() << "LogosAPI connected, making remote method call";
                    
                    // Make the remote method call
                    QVariant result = logosAPI->getClient(pluginNameStr)->invokeRemoteMethod(pluginNameStr, methodNameStr, args);
                    
                    QString resultMessage;
                    if (result.isValid()) {
                        // Convert result to string representation
                        QString resultStr;
                        if (result.canConvert<QString>()) {
                            resultStr = result.toString();
                        } else {
                            resultStr = QString("Result of type: %1").arg(result.typeName());
                        }
                        resultMessage = QString("Method call successful. Result: %1").arg(resultStr);
                        
                        QByteArray messageBytes = resultMessage.toUtf8();
                        callback(1, messageBytes.constData(), user_data);
                    } else {
                        resultMessage = QString("Method call returned invalid result");
                        QByteArray messageBytes = resultMessage.toUtf8();
                        callback(0, messageBytes.constData(), user_data);
                    }
                } else {
                    QString errorMsg = QString("Failed to connect to plugin: %1").arg(pluginNameStr);
                    QByteArray errorBytes = errorMsg.toUtf8();
                    callback(0, errorBytes.constData(), user_data);
                }
                
                // Clean up
                logosAPI->deleteLater();
                connectionTimer->deleteLater();
            });
            
            connectionTimer->start();
            
        } catch (const std::exception& e) {
            QString errorMsg = QString("Exception during method call: %1").arg(e.what());
            QByteArray errorBytes = errorMsg.toUtf8();
            callback(0, errorBytes.constData(), user_data);
        } catch (...) {
            QString errorMsg = QString("Unknown exception during method call");
            QByteArray errorBytes = errorMsg.toUtf8();
            callback(0, errorBytes.constData(), user_data);
        }
        
        // Clean up the main timer
        timer->deleteLater();
        });
        
    timer->start();
    qDebug() << "Async method call timer started for:" << pluginNameStr << "::" << methodNameStr;
}

void logos_core_register_event_listener(
    const char* plugin_name,
    const char* event_name, 
    AsyncCallback callback,
    void* user_data
) {
    if (!plugin_name || !event_name || !callback) {
        qWarning() << "logos_core_register_event_listener: Invalid parameters";
        return;
    }
    
    QString pluginNameStr = QString::fromUtf8(plugin_name);
    QString eventNameStr = QString::fromUtf8(event_name);
    
    qDebug() << "Registering event listener for plugin:" << pluginNameStr << "event:" << eventNameStr;
    
    // Check if plugin is loaded
    if (!g_loaded_plugins.contains(pluginNameStr)) {
        qWarning() << "Cannot register event listener: Plugin not loaded:" << pluginNameStr;
        return;
    }
    
    // Create event listener structure
    EventListener listener;
    listener.pluginName = pluginNameStr;
    listener.eventName = eventNameStr;
    listener.callback = callback;
    listener.userData = user_data;
    
    // Add to global list
    g_event_listeners.append(listener);
    
    // Set up the actual Qt Remote Objects event listener
    // Create timer with QCoreApplication as parent to ensure it's in the main thread
    QTimer* setupTimer = new QTimer(QCoreApplication::instance());
    setupTimer->setSingleShot(true);
    setupTimer->setInterval(1000); // Give plugin time to be ready
        
        QObject::connect(setupTimer, &QTimer::timeout, [=]() {
        // Create LogosAPI instance to connect to the plugin
        LogosAPI* logosAPI = new LogosAPI("core");
        
        // Use a delay to ensure connection is established
        QTimer* connectionTimer = new QTimer();
        connectionTimer->setSingleShot(true);
        connectionTimer->setInterval(2000); // 2 second delay
        
        QObject::connect(connectionTimer, &QTimer::timeout, [=]() {
            if (logosAPI->getClient(pluginNameStr)->isConnected()) {
                qDebug() << "LogosAPI connected for event listener, setting up event listener for" << eventNameStr;
                
                // Get the replica object to set up event listener
                QObject* replica = logosAPI->getClient(pluginNameStr)->requestObject(pluginNameStr);
                if (replica) {
                    // Set up event listener for the specified event
                    logosAPI->getClient(pluginNameStr)->onEvent(replica, nullptr, eventNameStr, [=](const QString& eventName, const QVariantList& eventData) {
                        qDebug() << "Event listener captured event:" << eventName << "with data:" << eventData;
                        
                        // Format the event data as JSON for the callback
                        QString eventResponse = QString("{\"event\":\"%1\",\"data\":[").arg(eventName);
                        for (int i = 0; i < eventData.size(); ++i) {
                            if (i > 0) eventResponse += ",";
                            eventResponse += QString("\"%1\"").arg(eventData[i].toString());
                        }
                        eventResponse += "]}";
                        
                        QByteArray eventBytes = eventResponse.toUtf8();
                        callback(1, eventBytes.constData(), user_data);
                    });
                    qDebug() << "Event listener successfully registered for" << pluginNameStr << "::" << eventNameStr;
                } else {
                    qWarning() << "Failed to get replica for event listener setup";
                }
            } else {
                qWarning() << "Failed to connect LogosAPI for event listener:" << pluginNameStr;
            }
            
            // Clean up timers but keep LogosAPI alive for the event listener
            connectionTimer->deleteLater();
        });
        
        connectionTimer->start();
        setupTimer->deleteLater();
        });
        
    setupTimer->start();
    qDebug() << "Event listener setup timer started for:" << pluginNameStr << "::" << eventNameStr;
}

static ProcessStats getProcessStats(qint64 pid)
{
    ProcessStats stats = {0.0, 0.0, 0.0};
    
    if (pid <= 0) {
        return stats;
    }
    
#if (defined(Q_OS_MACOS) || defined(Q_OS_MAC)) && !defined(Q_OS_IOS)
    // macOS implementation using libproc
    struct proc_taskinfo taskInfo;
    int ret = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &taskInfo, sizeof(taskInfo));
    
    if (ret == sizeof(taskInfo)) {
        // Get CPU time (user + system time) in microseconds, convert to seconds
        uint64_t totalTime = taskInfo.pti_total_user + taskInfo.pti_total_system;
        stats.cpuTimeSeconds = totalTime / 1e6;
        
        // Get memory footprint (resident size) in bytes, convert to megabytes
        stats.memoryMB = taskInfo.pti_resident_size / (1024.0 * 1024.0);
        
        // Calculate CPU percentage
        qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
        if (g_previous_cpu_times.contains(pid)) {
            QPair<double, qint64> previous = g_previous_cpu_times[pid];
            double timeDelta = (currentTime - previous.second) / 1000.0; // Convert to seconds
            double cpuDelta = stats.cpuTimeSeconds - previous.first;
            
            if (timeDelta > 0) {
                stats.cpuPercent = (cpuDelta / timeDelta) * 100.0;
            }
        }
        
        // Update previous values
        g_previous_cpu_times[pid] = QPair<double, qint64>(stats.cpuTimeSeconds, currentTime);
    }
    
#elif defined(Q_OS_LINUX)
    // Linux implementation using /proc filesystem
    QString statPath = QString("/proc/%1/stat").arg(pid);
    QString statusPath = QString("/proc/%1/status").arg(pid);
    
    // Read CPU time from /proc/[pid]/stat
    std::ifstream statFile(statPath.toStdString());
    if (statFile.is_open()) {
        std::string line;
        std::getline(statFile, line);
        std::istringstream iss(line);
        std::string token;
        
        // Skip first 13 tokens (pid, comm, state, ppid, etc.)
        // utime is token 14 (index 13), stime is token 15 (index 14)
        for (int i = 0; i < 14 && iss >> token; ++i) {}
        
        unsigned long utime = 0, stime = 0;
        if (iss >> utime && iss >> stime) {
            // CPU time is in clock ticks, convert to seconds
            long clockTicks = sysconf(_SC_CLK_TCK);
            if (clockTicks > 0) {
                stats.cpuTimeSeconds = (utime + stime) / static_cast<double>(clockTicks);
            }
        }
        statFile.close();
    }
    
    // Read memory from /proc/[pid]/status
    std::ifstream statusFile(statusPath.toStdString());
    if (statusFile.is_open()) {
        std::string line;
        while (std::getline(statusFile, line)) {
            if (line.find("VmRSS:") == 0) {
                // Extract memory value (in KB)
                std::istringstream iss(line);
                std::string label, value, unit;
                iss >> label >> value >> unit;
                if (!value.empty()) {
                    double memoryKB = std::stod(value);
                    stats.memoryMB = memoryKB / 1024.0;
                }
                break;
            }
        }
        statusFile.close();
    }
    
    // Calculate CPU percentage
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    if (g_previous_cpu_times.contains(pid)) {
        QPair<double, qint64> previous = g_previous_cpu_times[pid];
        double timeDelta = (currentTime - previous.second) / 1000.0; // Convert to seconds
        double cpuDelta = stats.cpuTimeSeconds - previous.first;
        
        if (timeDelta > 0) {
            stats.cpuPercent = (cpuDelta / timeDelta) * 100.0;
        }
    }
    
    // Update previous values
    g_previous_cpu_times[pid] = QPair<double, qint64>(stats.cpuTimeSeconds, currentTime);
    
#else
    // Unsupported platform
    qWarning() << "Process monitoring not supported on this platform";
#endif
    
    return stats;
}

char* logos_core_get_module_stats()
{
    qDebug() << "logos_core_get_module_stats() called";
    
    QJsonArray modulesArray;
    
#ifndef Q_OS_IOS
    // Iterate through plugin processes
    for (auto it = g_plugin_processes.begin(); it != g_plugin_processes.end(); ++it) {
        QString pluginName = it.key();
        QProcess* process = it.value();
        
        // Skip core_manager as it runs in-process
        if (pluginName == "core_manager") {
            continue;
        }
        
        // Get process ID
        qint64 pid = process->processId();
        if (pid <= 0) {
            qWarning() << "Invalid PID for plugin:" << pluginName;
            continue;
        }
        
        // Get process statistics
        ProcessStats stats = getProcessStats(pid);
        
        // Create JSON object for this module
        QJsonObject moduleObj;
        moduleObj["name"] = pluginName;
        moduleObj["cpu_percent"] = stats.cpuPercent;
        moduleObj["cpu_time_seconds"] = stats.cpuTimeSeconds;
        moduleObj["memory_mb"] = stats.memoryMB;
        
        modulesArray.append(moduleObj);
        
        qDebug() << "Module stats for" << pluginName 
                 << "- CPU:" << stats.cpuPercent << "%" 
                 << "(" << stats.cpuTimeSeconds << "s),"
                 << "Memory:" << stats.memoryMB << "MB";
    }
#endif // Q_OS_IOS
    
    // Convert to JSON string
    QJsonDocument doc(modulesArray);
    QByteArray jsonData = doc.toJson(QJsonDocument::Compact);
    
    // Allocate memory for the result string
    char* result = new char[jsonData.size() + 1];
    strcpy(result, jsonData.constData());
    
    qDebug() << "Returning module stats JSON for" << modulesArray.size() << "modules";
    
    return result;
}

void logos_core_process_events()
{
    if (g_app) {
        g_app->processEvents();
    }
} 
