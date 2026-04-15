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

#include "plugin_manager.h"
#include "plugin_registry.h"
#include "process_manager.h"
#include "qt/qt_token_receiver.h"

#include <QLocalServer>
#include <QString>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Plugin registry
// ---------------------------------------------------------------------------

inline void logos_core_register_plugin(const char* name, const char* path)
{
    if (!name || !path) return;
    PluginManager::registry().registerPlugin(std::string(name), std::string(path));
}

inline void logos_core_register_plugin_dependencies(const char* name,
                                                     const char** deps,
                                                     int count)
{
    if (!name) return;
    std::vector<std::string> stdDeps;
    for (int i = 0; i < count; ++i)
        if (deps && deps[i]) stdDeps.push_back(std::string(deps[i]));
    PluginManager::registry().registerDependencies(std::string(name), stdDeps);
}

inline int logos_core_is_plugin_known(const char* name)
{
    if (!name) return 0;
    return PluginManager::registry().isKnown(std::string(name)) ? 1 : 0;
}

inline int logos_core_is_plugin_loaded(const char* name)
{
    if (!name) return 0;
    return PluginManager::registry().isLoaded(std::string(name)) ? 1 : 0;
}

inline void logos_core_mark_plugin_loaded(const char* name)
{
    if (!name) return;
    PluginManager::registry().markLoaded(std::string(name));
}

inline char* logos_core_get_plugin_path(const char* name)
{
    if (!name) return nullptr;
    std::string path = PluginManager::registry().pluginPath(std::string(name));
    if (path.empty()) return nullptr;
    char* result = new char[path.size() + 1];
    memcpy(result, path.c_str(), path.size() + 1);
    return result;
}

inline int logos_core_get_plugin_dependencies_count(const char* name)
{
    if (!name) return 0;
    return static_cast<int>(
        PluginManager::registry().pluginDependencies(std::string(name)).size());
}

// ---------------------------------------------------------------------------
// Plugin directory queries
// ---------------------------------------------------------------------------

inline int logos_core_get_plugins_dirs_count()
{
    return static_cast<int>(PluginManager::registry().pluginsDirs().size());
}

inline char* logos_core_get_plugins_dir_at(int index)
{
    std::vector<std::string> dirs = PluginManager::registry().pluginsDirs();
    if (index < 0 || index >= static_cast<int>(dirs.size())) return nullptr;
    const std::string& s = dirs[static_cast<std::size_t>(index)];
    char* result = new char[s.size() + 1];
    memcpy(result, s.c_str(), s.size() + 1);
    return result;
}

// ---------------------------------------------------------------------------
// Dependency resolution
// ---------------------------------------------------------------------------

inline char** logos_core_resolve_dependencies(const char** names, int count)
{
    std::vector<std::string> requested;
    for (int i = 0; i < count; ++i)
        if (names && names[i]) requested.push_back(std::string(names[i]));

    std::vector<std::string> resolved = PluginManager::resolveDependencies(requested);

    std::size_t n = resolved.size();
    char** result = new char*[n + 1];
    for (std::size_t i = 0; i < n; ++i) {
        result[i] = new char[resolved[i].size() + 1];
        memcpy(result[i], resolved[i].c_str(), resolved[i].size() + 1);
    }
    result[n] = nullptr;
    return result;
}

// ---------------------------------------------------------------------------
// Lifecycle helpers (test teardown)
// ---------------------------------------------------------------------------

inline void logos_core_terminate_all()
{
    PluginManager::terminateAll();
}

inline void logos_core_clear()
{
    PluginManager::clear();
}

// ---------------------------------------------------------------------------
// Process management — QtProcessManager already uses std::string in its API;
// these pass-throughs keep the test call sites Qt-free.
// ---------------------------------------------------------------------------

inline void logos_core_register_process(const char* name)
{
    if (!name) return;
    QtProcessManager::registerProcess(std::string(name));
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
    QtProcessManager::ProcessCallbacks noopCallbacks;
    return QtProcessManager::startProcess(std::string(name),
                                          std::string(executable),
                                          arguments,
                                          noopCallbacks) ? 1 : 0;
}

inline int logos_core_send_token(const char* name, const char* token)
{
    if (!name || !token) return 0;
    return QtProcessManager::sendToken(std::string(name), std::string(token)) ? 1 : 0;
}

inline int logos_core_has_process(const char* name)
{
    if (!name) return 0;
    return QtProcessManager::hasProcess(std::string(name)) ? 1 : 0;
}

inline int64_t logos_core_get_process_id(const char* name)
{
    if (!name) return -1;
    return QtProcessManager::getProcessId(std::string(name));
}

inline void logos_core_terminate_process(const char* name)
{
    if (!name) return;
    QtProcessManager::terminateProcess(std::string(name));
}

inline void logos_core_clear_processes()
{
    QtProcessManager::clearAll();
}

// ---------------------------------------------------------------------------
// Token receiver
// ---------------------------------------------------------------------------

inline char* logos_core_receive_auth_token(const char* plugin_name)
{
    if (!plugin_name) {
        char* empty = new char[1];
        empty[0] = '\0';
        return empty;
    }
    std::string token = QtTokenReceiver::receiveAuthToken(std::string(plugin_name));
    char* result = new char[token.size() + 1];
    memcpy(result, token.c_str(), token.size() + 1);
    return result;
}

inline void logos_core_create_stale_token_socket(const char* plugin_name)
{
    if (!plugin_name) return;
    QString socketName = QString("logos_token_%1").arg(QString::fromUtf8(plugin_name));
    QLocalServer::removeServer(socketName);
    QLocalServer server;
    server.listen(socketName);
    server.close();
}
