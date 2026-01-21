#include "logos_core.h"
#include "logos_core_internal.h"
#include "plugin_manager.h"
#include "proxy_api.h"
#include "process_stats.h"
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
#include "common/interface.h"
#include "core_manager/core_manager.h"
#include "logos_api.h"
#include "logos_api_provider.h"
#include "logos_api_client.h"
#include "token_manager.h"
#include "logos_mode.h"

// Declare QObject* as a metatype so it can be stored in QVariant
Q_DECLARE_METATYPE(QObject*)

// === Global State Definitions ===

// Global application pointer
QCoreApplication* g_app = nullptr;

// Flag to track if we created the app or are using an existing one
bool g_app_created_by_us = false;

// Custom plugins directories (supports multiple directories)
QStringList g_plugins_dirs;

// Global list to store loaded plugin names
QStringList g_loaded_plugins;

// Global hash to store known plugin names and paths
QHash<QString, QString> g_known_plugins;

// Global hash to store plugin processes
#ifndef Q_OS_IOS
QHash<QString, QProcess*> g_plugin_processes;
#endif

// Global hash to store LogosAPI instances for Local mode plugins
QHash<QString, LogosAPI*> g_local_plugin_apis;

// Global Qt Remote Object registry host
QRemoteObjectRegistryHost* g_registry_host = nullptr;

// Global list to store registered event listeners
QList<EventListener> g_event_listeners;

// Global hash to track previous CPU times for percentage calculation
QHash<qint64, QPair<double, qint64>> g_previous_cpu_times;

// === C API Implementation ===

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
    g_plugins_dirs.clear();
    if (plugins_dir) {
        g_plugins_dirs.append(QString(plugins_dir));
        qDebug() << "Custom plugins directory set to:" << g_plugins_dirs.first();
    }
}

void logos_core_add_plugins_dir(const char* plugins_dir)
{
    if (plugins_dir) {
        QString dir = QString(plugins_dir);
        if (!g_plugins_dirs.contains(dir)) {
            g_plugins_dirs.append(dir);
            qDebug() << "Added plugins directory:" << dir;
        }
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
    if (!PluginManager::initializeCoreManager()) {
        qWarning() << "Failed to initialize core manager, continuing with other modules...";
    }
    
    // Define the plugins directories to scan
    QStringList pluginsDirs;
    if (!g_plugins_dirs.isEmpty()) {
        // Use the custom plugins directories if set
        pluginsDirs = g_plugins_dirs;
    } else {
        // Use the default plugins directory
        pluginsDirs << QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../modules");
    }
    
    qDebug() << "Looking for modules in" << pluginsDirs.size() << "directories:" << pluginsDirs;
    
    // Find and process all plugins in all directories to populate g_known_plugins
    for (const QString& pluginsDir : pluginsDirs) {
        qDebug() << "Scanning directory:" << pluginsDir;
        QStringList pluginPaths = PluginManager::findPlugins(pluginsDir);
        
        if (pluginPaths.isEmpty()) {
            qDebug() << "No modules found in:" << pluginsDir;
        } else {
            qDebug() << "Found" << pluginPaths.size() << "modules in:" << pluginsDir;
            
            // Process each plugin to add to known plugins list
            for (const QString &pluginPath : pluginPaths) {
                QString pluginName = PluginManager::processPlugin(pluginPath);
                if (pluginName.isEmpty()) {
                    qWarning() << "Failed to process plugin (no metadata or invalid):" << pluginPath;
                } else {
                    qDebug() << "Successfully processed plugin:" << pluginName;
                }
            }
        }
    }
    
    qDebug() << "Total known plugins after processing:" << g_known_plugins.size();
    qDebug() << "Known plugin names:" << g_known_plugins.keys();

    // Initialize capability module if available (after plugin discovery)
    PluginManager::initializeCapabilityModule();
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
    bool success = PluginManager::loadPlugin(name);
    return success ? 1 : 0;
}

int logos_core_load_static_plugins()
{
    if (!LogosModeConfig::isLocal()) {
        qWarning() << "logos_core_load_static_plugins() requires Local mode. Call logos_core_set_mode(LOGOS_MODE_LOCAL) first.";
        return 0;
    }

    return PluginManager::loadStaticPlugins();
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
    bool success = PluginManager::registerPluginInstance(pluginName, plugin_instance);
    return success ? 1 : 0;
}

int logos_core_register_plugin_by_name(const char* plugin_name)
{
    if (!plugin_name) {
        qWarning() << "logos_core_register_plugin_by_name: plugin_name is null";
        return 0;
    }
    
    if (!LogosModeConfig::isLocal()) {
        qWarning() << "logos_core_register_plugin_by_name() requires Local mode. Call logos_core_set_mode(LOGOS_MODE_LOCAL) first.";
        return 0;
    }
    
    QString name = QString::fromUtf8(plugin_name);
    bool success = PluginManager::registerPluginByName(name);
    return success ? 1 : 0;
}

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
    bool success = PluginManager::unloadPlugin(name);
    return success ? 1 : 0;
#endif // Q_OS_IOS
}

char* logos_core_process_plugin(const char* plugin_path)
{
    if (!plugin_path) {
        qWarning() << "Cannot process plugin: path is null";
        return nullptr;
    }

    QString path = QString::fromUtf8(plugin_path);
    qDebug() << "Processing plugin file:" << path;

    QString pluginName = PluginManager::processPlugin(path);
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

char* logos_core_get_module_stats()
{
    return ProcessStats::getModuleStats();
}

// === Async Callback API Implementation ===

void logos_core_async_operation(const char* data, AsyncCallback callback, void* user_data)
{
    ProxyAPI::asyncOperation(data, callback, user_data);
}

void logos_core_load_plugin_async(const char* plugin_name, AsyncCallback callback, void* user_data)
{
    ProxyAPI::loadPluginAsync(plugin_name, callback, user_data);
}

void logos_core_call_plugin_method_async(
    const char* plugin_name, 
    const char* method_name, 
    const char* params_json, 
    AsyncCallback callback, 
    void* user_data
) {
    ProxyAPI::callPluginMethodAsync(plugin_name, method_name, params_json, callback, user_data);
}

void logos_core_register_event_listener(
    const char* plugin_name,
    const char* event_name, 
    AsyncCallback callback,
    void* user_data
) {
    ProxyAPI::registerEventListener(plugin_name, event_name, callback, user_data);
}

void logos_core_process_events()
{
    if (g_app) {
        g_app->processEvents();
    }
}
