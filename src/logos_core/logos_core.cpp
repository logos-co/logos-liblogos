#include "logos_core.h"
#include "plugin_manager.h"
#include <logos_instance.h>
#include <process_stats/process_stats.h>
#include "token_manager.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

// === C API Implementation (Thin Wrappers) ===

void logos_core_init(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
}

void logos_core_set_plugins_dir(const char* plugins_dir) {
    PluginManager::setPluginsDir(plugins_dir);
}

void logos_core_add_plugins_dir(const char* plugins_dir) {
    PluginManager::addPluginsDir(plugins_dir);
}

void logos_core_start() {
    LogosInstance::id();
    PluginManager::discoverInstalledModules();
    PluginManager::initializeCapabilityModule();
}

int logos_core_exec() {
    return 0;
}

void logos_core_cleanup() {
    PluginManager::clear();
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

int logos_core_unload_plugin_with_dependents(const char* plugin_name) {
    if (!plugin_name) { fprintf(stderr, "logos_core_unload_plugin_with_dependents: plugin_name must not be null\n"); std::abort(); }
    return PluginManager::unloadPluginWithDependents(plugin_name) ? 1 : 0;
}

char** logos_core_get_module_dependencies(const char* module_name, bool recursive) {
    if (!module_name) { fprintf(stderr, "logos_core_get_module_dependencies: module_name must not be null\n"); std::abort(); }
    return PluginManager::getDependenciesCStr(module_name, recursive);
}

char** logos_core_get_module_dependents(const char* module_name, bool recursive) {
    if (!module_name) { fprintf(stderr, "logos_core_get_module_dependents: module_name must not be null\n"); std::abort(); }
    return PluginManager::getDependentsCStr(module_name, recursive);
}

char* logos_core_process_plugin(const char* plugin_path) {
    if (!plugin_path) { fprintf(stderr, "logos_core_process_plugin: plugin_path must not be null\n"); std::abort(); }
    return PluginManager::processPluginCStr(plugin_path);
}

char* logos_core_get_token(const char* key) {
    if (!key) { fprintf(stderr, "logos_core_get_token: key must not be null\n"); std::abort(); }

    std::string token = TokenManager::instance().getToken(key).toStdString();
    if (token.empty()) return nullptr;

    char* result = new char[token.size() + 1];
    memcpy(result, token.c_str(), token.size() + 1);
    return result;
}

char* logos_core_get_module_stats() {
    return ProcessStats::getModuleStats(PluginManager::getPluginProcessIds());
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
}
