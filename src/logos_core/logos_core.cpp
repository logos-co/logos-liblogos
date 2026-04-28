#include "logos_core.h"
#include "module_manager.h"
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

void logos_core_set_modules_dir(const char* modules_dir) {
    ModuleManager::setModulesDir(modules_dir);
}

void logos_core_add_modules_dir(const char* modules_dir) {
    ModuleManager::addModulesDir(modules_dir);
}

void logos_core_start() {
    LogosInstance::id();
    ModuleManager::discoverInstalledModules();
    ModuleManager::initializeCapabilityModule();
}

int logos_core_exec() {
    return 0;
}

void logos_core_cleanup() {
    ModuleManager::clear();
}

char** logos_core_get_loaded_modules() {
    return ModuleManager::getLoadedModulesCStr();
}

char** logos_core_get_known_modules() {
    return ModuleManager::getKnownModulesCStr();
}

int logos_core_load_module(const char* module_name) {
    if (!module_name) { fprintf(stderr, "logos_core_load_module: module_name must not be null\n"); std::abort(); }
    return ModuleManager::loadModule(module_name) ? 1 : 0;
}

int logos_core_load_module_with_dependencies(const char* module_name) {
    if (!module_name) { fprintf(stderr, "logos_core_load_module_with_dependencies: module_name must not be null\n"); std::abort(); }
    return ModuleManager::loadModuleWithDependencies(module_name) ? 1 : 0;
}

int logos_core_unload_module(const char* module_name) {
    if (!module_name) { fprintf(stderr, "logos_core_unload_module: module_name must not be null\n"); std::abort(); }
    return ModuleManager::unloadModule(module_name) ? 1 : 0;
}

int logos_core_unload_module_with_dependents(const char* module_name) {
    if (!module_name) { fprintf(stderr, "logos_core_unload_module_with_dependents: module_name must not be null\n"); std::abort(); }
    return ModuleManager::unloadModuleWithDependents(module_name) ? 1 : 0;
}

char** logos_core_get_module_dependencies(const char* module_name, bool recursive) {
    if (!module_name) { fprintf(stderr, "logos_core_get_module_dependencies: module_name must not be null\n"); std::abort(); }
    return ModuleManager::getDependenciesCStr(module_name, recursive);
}

char** logos_core_get_module_dependents(const char* module_name, bool recursive) {
    if (!module_name) { fprintf(stderr, "logos_core_get_module_dependents: module_name must not be null\n"); std::abort(); }
    return ModuleManager::getDependentsCStr(module_name, recursive);
}

char* logos_core_process_module(const char* module_path) {
    if (!module_path) { fprintf(stderr, "logos_core_process_module: module_path must not be null\n"); std::abort(); }
    return ModuleManager::processModuleCStr(module_path);
}

char* logos_core_get_token(const char* key) {
    if (!key) { fprintf(stderr, "logos_core_get_token: key must not be null\n"); std::abort(); }

    std::string token = TokenManager::instance().getToken(std::string(key));
    if (token.empty()) return nullptr;

    char* result = new char[token.size() + 1];
    memcpy(result, token.c_str(), token.size() + 1);
    return result;
}

char* logos_core_get_module_stats() {
    return ProcessStats::getModuleStats(ModuleManager::getModuleProcessIds());
}

void logos_core_set_persistence_base_path(const char* path) {
    if (!path) { fprintf(stderr, "logos_core_set_persistence_base_path: path must not be null\n"); std::abort(); }
    ModuleManager::setPersistenceBasePath(path);
}

void logos_core_set_module_transports(const char* module_name,
                                       const char* transport_set_json) {
    if (!module_name) {
        fprintf(stderr, "logos_core_set_module_transports: module_name must not be null\n");
        std::abort();
    }
    ModuleManager::setModuleTransports(
        std::string(module_name),
        transport_set_json ? std::string(transport_set_json) : std::string{});
}

void logos_core_refresh_modules()
{
    ModuleManager::discoverInstalledModules();
}


void logos_core_process_events()
{
}

// =========================================================================
// DEPRECATED wrappers — forward to the new logos_core_*_module* functions.
// =========================================================================

void logos_core_set_plugins_dir(const char* plugins_dir) {
    fprintf(stderr, "DEPRECATED: logos_core_set_plugins_dir — use logos_core_set_modules_dir\n");
    logos_core_set_modules_dir(plugins_dir);
}

void logos_core_add_plugins_dir(const char* plugins_dir) {
    fprintf(stderr, "DEPRECATED: logos_core_add_plugins_dir — use logos_core_add_modules_dir\n");
    logos_core_add_modules_dir(plugins_dir);
}

char** logos_core_get_loaded_plugins() {
    fprintf(stderr, "DEPRECATED: logos_core_get_loaded_plugins — use logos_core_get_loaded_modules\n");
    return logos_core_get_loaded_modules();
}

char** logos_core_get_known_plugins() {
    fprintf(stderr, "DEPRECATED: logos_core_get_known_plugins — use logos_core_get_known_modules\n");
    return logos_core_get_known_modules();
}

int logos_core_load_plugin(const char* plugin_name) {
    fprintf(stderr, "DEPRECATED: logos_core_load_plugin — use logos_core_load_module\n");
    return logos_core_load_module(plugin_name);
}

int logos_core_load_plugin_with_dependencies(const char* plugin_name) {
    fprintf(stderr, "DEPRECATED: logos_core_load_plugin_with_dependencies — use logos_core_load_module_with_dependencies\n");
    return logos_core_load_module_with_dependencies(plugin_name);
}

int logos_core_unload_plugin(const char* plugin_name) {
    fprintf(stderr, "DEPRECATED: logos_core_unload_plugin — use logos_core_unload_module\n");
    return logos_core_unload_module(plugin_name);
}

int logos_core_unload_plugin_with_dependents(const char* plugin_name) {
    fprintf(stderr, "DEPRECATED: logos_core_unload_plugin_with_dependents — use logos_core_unload_module_with_dependents\n");
    return logos_core_unload_module_with_dependents(plugin_name);
}

char* logos_core_process_plugin(const char* plugin_path) {
    fprintf(stderr, "DEPRECATED: logos_core_process_plugin — use logos_core_process_module\n");
    return logos_core_process_module(plugin_path);
}

void logos_core_refresh_plugins() {
    fprintf(stderr, "DEPRECATED: logos_core_refresh_plugins — use logos_core_refresh_modules\n");
    logos_core_refresh_modules();
}
