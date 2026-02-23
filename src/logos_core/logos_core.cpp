#include "logos_core.h"
#include "logos_core_internal.h"
#include "app_lifecycle.h"
#include "plugin_manager.h"
#include "proxy_api.h"
#include <process_stats/process_stats.h>
#include "token_manager.h"
#include <QDebug>
#include <QByteArray>
#include <cstring>

// === C API Implementation (Thin Wrappers) ===

void logos_core_init(int argc, char *argv[])
{
    AppLifecycle::init(argc, argv);
}

void logos_core_set_mode(int mode)
{
    AppLifecycle::setMode(mode);
}

void logos_core_set_plugins_dir(const char* plugins_dir)
{
    AppLifecycle::setPluginsDir(plugins_dir);
}

void logos_core_add_plugins_dir(const char* plugins_dir)
{
    AppLifecycle::addPluginsDir(plugins_dir);
}

void logos_core_start()
{
    AppLifecycle::start();
}

int logos_core_exec()
{
    return AppLifecycle::exec();
}

void logos_core_cleanup()
{
    AppLifecycle::cleanup();
}

char** logos_core_get_loaded_plugins()
{
    return PluginManager::getLoadedPluginsCStr();
}

char** logos_core_get_known_plugins()
{
    return PluginManager::getKnownPluginsCStr();
}

int logos_core_load_plugin(const char* plugin_name)
{
    if (!plugin_name) {
        qWarning() << "Cannot load plugin: name is null";
        return 0;
    }
    
    QString name = QString::fromUtf8(plugin_name);
    bool success = PluginManager::loadPlugin(name);
    return success ? 1 : 0;
}

int logos_core_load_plugin_with_dependencies(const char* plugin_name)
{
    if (!plugin_name) {
        qWarning() << "Cannot load plugin: name is null";
        return 0;
    }
    
    QString name = QString::fromUtf8(plugin_name);
    QStringList requestedModules;
    requestedModules.append(name);
    
    QStringList resolvedModules = PluginManager::resolveDependencies(requestedModules);
    
    // If the requested plugin wasn't resolved (unknown plugin), return failure
    if (resolvedModules.isEmpty() || !resolvedModules.contains(name)) {
        qWarning() << "Cannot load plugin: plugin not found:" << name;
        return 0;
    }
    
    qDebug() << "Loading plugin with resolved dependencies:" << resolvedModules;
    
    // Load all plugins in dependency order
    bool allSucceeded = true;
    for (const QString& moduleName : resolvedModules) {
        if (PluginManager::isPluginLoaded(moduleName)) {
            qDebug() << "Plugin already loaded, skipping:" << moduleName;
            continue;
        }
        if (!PluginManager::loadPlugin(moduleName)) {
            qWarning() << "Failed to load module:" << moduleName;
            allSucceeded = false;
        }
    }
    
    return allSucceeded ? 1 : 0;
}

int logos_core_load_static_plugins()
{
    return PluginManager::loadStaticPlugins();
}

int logos_core_register_plugin_instance(const char* plugin_name, void* plugin_instance)
{
    if (!plugin_name || !plugin_instance) {
        qWarning() << "logos_core_register_plugin_instance: Invalid arguments (name or instance is null)";
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
#ifdef Q_OS_IOS
    // iOS doesn't support process-based plugins, pass empty map
    QHash<QString, qint64> emptyProcesses;
    return ProcessStats::getModuleStats(emptyProcesses);
#else
    // Build PID map from plugin processes
    QHash<QString, qint64> processes;
    for (auto it = g_plugin_processes.begin(); it != g_plugin_processes.end(); ++it) {
        // Skip core_manager as it runs in-process
        if (it.key() == "core_manager") {
            continue;
        }
        processes[it.key()] = it.value()->processId();
    }
    return ProcessStats::getModuleStats(processes);
#endif
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
    AppLifecycle::processEvents();
}
