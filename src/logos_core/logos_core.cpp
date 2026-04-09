#include "logos_core.h"
#include "app_lifecycle.h"
#include "plugin_manager.h"
#include <process_stats/process_stats.h>
#include "token_manager.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace {

    std::unordered_map<std::string, int64_t> pluginPidsForProcessStats()
    {
        const QHash<QString, qint64> q = PluginManager::getPluginProcessIds();
        std::unordered_map<std::string, int64_t> out;
        out.reserve(static_cast<size_t>(q.size()));
        for (auto it = q.constBegin(); it != q.constEnd(); ++it)
            out[it.key().toStdString()] = static_cast<int64_t>(it.value());
        return out;
    }

}

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
    if (!plugin_name) { fprintf(stderr, "logos_core_load_plugin: plugin_name must not be null\n"); std::abort(); }
    return PluginManager::loadPlugin(plugin_name) ? 1 : 0;
}

int logos_core_load_plugin_with_dependencies(const char* plugin_name) {
    if (!plugin_name) { fprintf(stderr, "logos_core_load_plugin_with_dependencies: plugin_name must not be null\n"); std::abort(); }
    return PluginManager::loadPluginWithDependencies(plugin_name) ? 1 : 0;
}

int logos_core_unload_plugin(const char* plugin_name) {
    if (!plugin_name) { fprintf(stderr, "logos_core_unload_plugin: plugin_name must not be null\n"); std::abort(); }
    return PluginManager::unloadPlugin(plugin_name) ? 1 : 0;
}

char* logos_core_process_plugin(const char* plugin_path) {
    if (!plugin_path) { fprintf(stderr, "logos_core_process_plugin: plugin_path must not be null\n"); std::abort(); }
    return PluginManager::processPluginCStr(plugin_path);
}

char* logos_core_get_token(const char* key) {
    if (!key) { fprintf(stderr, "logos_core_get_token: key must not be null\n"); std::abort(); }

    QString token = TokenManager::instance().getToken(QString::fromUtf8(key));
    if (token.isEmpty()) return nullptr;

    std::string utf8 = token.toStdString();
    char* result = new char[utf8.size() + 1];
    memcpy(result, utf8.c_str(), utf8.size() + 1);
    return result;
}

char* logos_core_get_module_stats() {
    return ProcessStats::getModuleStats(pluginPidsForProcessStats());
}

void logos_core_set_persistence_base_path(const char* path) {
    if (!path) { fprintf(stderr, "logos_core_set_persistence_base_path: path must not be null\n"); std::abort(); }
    PluginManager::setPersistenceBasePath(path);
}

void logos_core_refresh_plugins()
{
    PluginManager::discoverInstalledModules();
}


void logos_core_process_events()
{
    AppLifecycle::processEvents();
}
