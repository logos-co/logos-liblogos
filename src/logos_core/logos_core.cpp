#include "logos_core.h"
#include "app_lifecycle.h"
#include "plugin_manager.h"
#include <process_stats/process_stats.h>
#include "token_manager.h"
#include <QDebug>
#include <QByteArray>
#include <QStringList>
#include <cstring>

// === C API Implementation (Thin Wrappers) ===

void logos_core_init(int argc, char *argv[]) {
    AppLifecycle::init(argc, argv);
}

void logos_core_set_plugins_dir(const char* plugins_dir) {
    PluginManager::setPluginsDir(plugins_dir);
}

void logos_core_add_plugins_dir(const char* plugins_dir) {
    PluginManager::addPluginsDir(plugins_dir);
}

void logos_core_start() {
    AppLifecycle::start();
}

int logos_core_exec() {
    return AppLifecycle::exec();
}

void logos_core_cleanup() {
    AppLifecycle::cleanup();
}

char** logos_core_get_loaded_plugins() {
    return PluginManager::getLoadedPluginsCStr();
}

char** logos_core_get_known_plugins() {
    return PluginManager::getKnownPluginsCStr();
}

int logos_core_load_plugin(const char* plugin_name) {
    if (!plugin_name) qFatal("logos_core_load_plugin: plugin_name must not be null");
    
    QString name = QString::fromUtf8(plugin_name);
    bool success = PluginManager::loadPlugin(name);
    return success ? 1 : 0;
}

int logos_core_load_plugin_with_dependencies(const char* plugin_name) {
    if (!plugin_name) qFatal("logos_core_load_plugin_with_dependencies: plugin_name must not be null");
    
    QString name = QString::fromUtf8(plugin_name);
    QStringList requestedModules;
    requestedModules.append(name);
    
    QStringList resolvedModules = PluginManager::resolveDependencies(requestedModules);
    
    // If the requested plugin wasn't resolved (unknown plugin), return failure
    if (resolvedModules.isEmpty() || !resolvedModules.contains(name)) {
        qWarning() << "Cannot load plugin: plugin not found:" << name;
        return 0;
    }

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

int logos_core_unload_plugin(const char* plugin_name) {
    if (!plugin_name) qFatal("logos_core_unload_plugin: plugin_name must not be null");

    QString name = QString::fromUtf8(plugin_name);
    bool success = PluginManager::unloadPlugin(name);
    return success ? 1 : 0;
}

char* logos_core_process_plugin(const char* plugin_path) {
    if (!plugin_path) qFatal("logos_core_process_plugin: plugin_path must not be null");

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

char* logos_core_get_token(const char* key) {
    if (!key) qFatal("logos_core_get_token: key must not be null");

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

char* logos_core_get_module_stats() {
    return ProcessStats::getModuleStats(PluginManager::getPluginProcessIds());
}

void logos_core_refresh_plugins()
{
    PluginManager::discoverInstalledModules();
}


void logos_core_process_events()
{
    AppLifecycle::processEvents();
}
