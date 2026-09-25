// ---------------------------------------------------------------------------
// Qt-isolating adapter for logos_core tests.
//
// Provides the same logos_core_* names as the removed C API extensions,
// implemented as inline wrappers around the internal classes.
//
// Test .cpp files include this header and never import Qt headers directly.
// When Qt is replaced in src/, update only this file to call the new internals.
// The test assertions themselves remain unchanged.
// ---------------------------------------------------------------------------
#pragma once

#include "module_manager.h"
#include "module_registry.h"
#include "stand_in_authority.h"
#include "subprocess_manager.h"

#include "logos_protocol.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Module registry
// ---------------------------------------------------------------------------

inline void logos_core_register_module(const char* name, const char* path)
{
    if (!name || !path) return;
    ModuleManager::registry().registerModule(std::string(name), std::string(path));
}

inline void logos_core_register_module_dependencies(const char* name,
                                                     const char** deps,
                                                     int count)
{
    if (!name) return;
    std::vector<std::string> stdDeps;
    for (int i = 0; i < count; ++i)
        if (deps && deps[i]) stdDeps.push_back(std::string(deps[i]));
    ModuleManager::registry().registerDependencies(std::string(name), stdDeps);
}

inline void logos_core_register_module_optional_dependencies(const char* name,
                                                             const char** deps,
                                                             int count)
{
    if (!name) return;
    std::vector<std::string> stdDeps;
    for (int i = 0; i < count; ++i)
        if (deps && deps[i]) stdDeps.push_back(std::string(deps[i]));
    ModuleManager::registry().registerOptionalDependencies(std::string(name), stdDeps);
}

inline char** logos_core_get_module_optional_dependencies_test(const char* name)
{
    if (!name) return nullptr;
    return ModuleManager::getOptionalDependenciesCStr(name);
}

inline int logos_core_is_module_known(const char* name)
{
    if (!name) return 0;
    return ModuleManager::registry().isKnown(std::string(name)) ? 1 : 0;
}

inline int logos_core_is_module_loaded(const char* name)
{
    if (!name) return 0;
    return ModuleManager::registry().isLoaded(std::string(name)) ? 1 : 0;
}

inline void logos_core_mark_module_loaded(const char* name)
{
    if (!name) return;
    ModuleManager::registry().markLoaded(std::string(name));
}

inline char* logos_core_get_module_path(const char* name)
{
    if (!name) return nullptr;
    std::string path = ModuleManager::registry().modulePath(std::string(name));
    if (path.empty()) return nullptr;
    char* result = new char[path.size() + 1];
    memcpy(result, path.c_str(), path.size() + 1);
    return result;
}

inline int logos_core_get_module_dependencies_count(const char* name)
{
    if (!name) return 0;
    return static_cast<int>(
        ModuleManager::registry().moduleDependencies(std::string(name)).size());
}

// ---------------------------------------------------------------------------
// Module directory queries
// ---------------------------------------------------------------------------

inline int logos_core_get_modules_dirs_count()
{
    return static_cast<int>(ModuleManager::registry().modulesDirs().size());
}

inline char* logos_core_get_modules_dir_at(int index)
{
    std::vector<std::string> dirs = ModuleManager::registry().modulesDirs();
    if (index < 0 || index >= static_cast<int>(dirs.size())) return nullptr;
    const std::string& s = dirs[static_cast<std::size_t>(index)];
    char* result = new char[s.size() + 1];
    memcpy(result, s.c_str(), s.size() + 1);
    return result;
}

// ---------------------------------------------------------------------------
// Dependency resolution
// ---------------------------------------------------------------------------

// The null-terminated char** every resolve shim below hands back. Extracted
// when the best-effort shims arrived rather than copied a third time.
inline char** toCArray(const std::vector<std::string>& v)
{
    std::size_t n = v.size();
    char** result = new char*[n + 1];
    for (std::size_t i = 0; i < n; ++i) {
        result[i] = new char[v[i].size() + 1];
        memcpy(result[i], v[i].c_str(), v[i].size() + 1);
    }
    result[n] = nullptr;
    return result;
}

// Best-effort resolution, for the tests that need to see BOTH halves of the
// answer. Two shims over one call rather than an out-parameter: a test reads
// one property at a time, and the C-array marshalling is already the noisiest
// thing in this file.
inline char** logos_core_resolve_dependencies_best_effort(const char** names, int count)
{
    std::vector<std::string> requested;
    for (int i = 0; i < count; ++i)
        if (names && names[i]) requested.push_back(std::string(names[i]));
    return toCArray(ModuleManager::resolveDependenciesBestEffort(requested).order);
}

// The subset of the above whose load failure a caller must tolerate.
inline char** logos_core_resolve_best_effort_names(const char** names, int count)
{
    std::vector<std::string> requested;
    for (int i = 0; i < count; ++i)
        if (names && names[i]) requested.push_back(std::string(names[i]));
    return toCArray(ModuleManager::resolveDependenciesBestEffort(requested).bestEffort);
}

inline char** logos_core_resolve_dependencies(const char** names, int count)
{
    std::vector<std::string> requested;
    for (int i = 0; i < count; ++i)
        if (names && names[i]) requested.push_back(std::string(names[i]));

    return toCArray(ModuleManager::resolveDependencies(requested));
}

// ---------------------------------------------------------------------------
// Lifecycle helpers (test teardown)
// ---------------------------------------------------------------------------

inline void logos_core_terminate_all()
{
    ModuleManager::terminateAll();
    // The authority goes with the fleet; the stand-in admits what loads next.
    stand_in::attach();
}

inline void logos_core_clear()
{
    ModuleManager::clear();
    // clear() detaches the authority: the next case is admitted by the stand-in.
    stand_in::attach();
}

// ---------------------------------------------------------------------------
// The lifecycle calls that left the C API for core_service, over the module
// manager, so the load-path cases keep their call sites. Test-only.
// ---------------------------------------------------------------------------

typedef enum {
    LOGOS_LOAD_MODULE_ONLY = 0,
    LOGOS_LOAD_REQUIRED_DEPS = 1,
    LOGOS_LOAD_REQUIRED_AND_OPTIONAL = 2,
} LogosLoadDeps;

inline int logos_core_load_module(const char* name, LogosLoadDeps deps)
{
    switch (deps) {
    case LOGOS_LOAD_REQUIRED_AND_OPTIONAL:
        return ModuleManager::loadModuleWithDependencies(
                   name, DependencyResolver::OptionalLoad::BestEffort) ? 1 : 0;
    case LOGOS_LOAD_REQUIRED_DEPS:
        return ModuleManager::loadModuleWithDependencies(
                   name, DependencyResolver::OptionalLoad::OrderOnly) ? 1 : 0;
    case LOGOS_LOAD_MODULE_ONLY:
        return ModuleManager::loadModule(name) ? 1 : 0;
    }
    return 0;
}

inline int logos_core_unload_module(const char* name, bool withDependents)
{
    return (withDependents ? ModuleManager::unloadModuleWithDependents(name)
                           : ModuleManager::unloadModule(name)) ? 1 : 0;
}

inline char** logos_core_get_loaded_modules() { return ModuleManager::getLoadedModulesCStr(); }
inline char** logos_core_get_known_modules() { return ModuleManager::getKnownModulesCStr(); }

inline char** logos_core_get_module_dependencies(const char* name, bool recursive)
{
    return ModuleManager::getDependenciesCStr(name, recursive);
}

inline char** logos_core_get_module_dependents(const char* name, bool recursive)
{
    return ModuleManager::getDependentsCStr(name, recursive);
}

inline char* logos_core_get_modules_info() { return ModuleManager::getModulesInfoCStr(); }

inline char* logos_core_optional_load_report(const char* name)
{
    return ModuleManager::optionalLoadReportCStr(name);
}

inline void logos_core_refresh_modules() { ModuleManager::discoverInstalledModules(); }

// Core's own token for `key`, as its clients present it; free with delete[].
inline char* logos_core_get_token(const char* key)
{
    char* stored = lp_token_get(key);
    if (!stored) return nullptr;
    const std::string token = stored;
    lp_string_free(stored);
    char* out = new char[token.size() + 1];
    std::memcpy(out, token.c_str(), token.size() + 1);
    return out;
}

// ---------------------------------------------------------------------------
// Process management — SubprocessManager uses std::string throughout;
// these pass-throughs keep the test call sites Qt-free.
// ---------------------------------------------------------------------------

inline void logos_core_register_process(const char* name)
{
    if (!name) return;
    SubprocessManager::registerProcess(std::string(name));
}

inline int logos_core_start_process(const char* name,
                                     const char* executable,
                                     const char** args)
{
    if (!name || !executable) return 0;
    std::vector<std::string> arguments;
    if (args)
        for (int i = 0; args[i] != nullptr; ++i)
            arguments.push_back(args[i]);
    SubprocessManager::ProcessCallbacks noopCallbacks;
    return SubprocessManager::startProcess(std::string(name),
                                           std::string(executable),
                                           arguments,
                                           noopCallbacks) ? 1 : 0;
}

inline int logos_core_has_process(const char* name)
{
    if (!name) return 0;
    return SubprocessManager::hasProcess(std::string(name)) ? 1 : 0;
}

inline int64_t logos_core_get_process_id(const char* name)
{
    if (!name) return -1;
    return SubprocessManager::getProcessId(std::string(name));
}

inline void logos_core_terminate_process(const char* name)
{
    if (!name) return;
    SubprocessManager::terminateProcess(std::string(name));
}

inline void logos_core_clear_processes()
{
    SubprocessManager::clearAll();
}
