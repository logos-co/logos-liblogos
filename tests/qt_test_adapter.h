// ---------------------------------------------------------------------------
// Qt-isolating adapter for logos_core tests.
//
// Provides the same logos_core_* names as the removed C API extensions,
// implemented as inline wrappers around the Qt-typed internal classes.
//
// Test .cpp files include this header and never import Qt headers directly.
// When Qt is replaced in src/, update only this file to call the new internals.
// The test assertions themselves remain unchanged.
// ---------------------------------------------------------------------------
#pragma once

#include "plugin_manager.h"
#include "plugin_registry.h"
#include "qt/qt_process_manager.h"
#include "qt/qt_token_receiver.h"

#include <QLocalServer>
#include <QString>
#include <QStringList>

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
    PluginManager::registry().registerPlugin(QString::fromUtf8(name),
                                             QString::fromUtf8(path));
}

inline void logos_core_register_plugin_dependencies(const char* name,
                                                     const char** deps,
                                                     int count)
{
    if (!name) return;
    QStringList qDeps;
    for (int i = 0; i < count; ++i)
        if (deps && deps[i]) qDeps.append(QString::fromUtf8(deps[i]));
    PluginManager::registry().registerDependencies(QString::fromUtf8(name), qDeps);
}

inline int logos_core_is_plugin_known(const char* name)
{
    if (!name) return 0;
    return PluginManager::registry().isKnown(QString::fromUtf8(name)) ? 1 : 0;
}

inline int logos_core_is_plugin_loaded(const char* name)
{
    if (!name) return 0;
    return PluginManager::registry().isLoaded(QString::fromUtf8(name)) ? 1 : 0;
}

inline void logos_core_mark_plugin_loaded(const char* name)
{
    if (!name) return;
    PluginManager::registry().markLoaded(QString::fromUtf8(name));
}

inline char* logos_core_get_plugin_path(const char* name)
{
    if (!name) return nullptr;
    QString path = PluginManager::registry().pluginPath(QString::fromUtf8(name));
    if (path.isEmpty()) return nullptr;
    std::string s = path.toStdString();
    char* result = new char[s.size() + 1];
    memcpy(result, s.c_str(), s.size() + 1);
    return result;
}

inline int logos_core_get_plugin_dependencies_count(const char* name)
{
    if (!name) return 0;
    return static_cast<int>(
        PluginManager::registry().pluginDependencies(QString::fromUtf8(name)).size());
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
    QStringList dirs = PluginManager::registry().pluginsDirs();
    if (index < 0 || index >= dirs.size()) return nullptr;
    std::string s = dirs[index].toStdString();
    char* result = new char[s.size() + 1];
    memcpy(result, s.c_str(), s.size() + 1);
    return result;
}

// ---------------------------------------------------------------------------
// Dependency resolution
// ---------------------------------------------------------------------------

inline char** logos_core_resolve_dependencies(const char** names, int count)
{
    QStringList requested;
    for (int i = 0; i < count; ++i)
        if (names && names[i]) requested.append(QString::fromUtf8(names[i]));

    QStringList resolved = PluginManager::resolveDependencies(requested);

    int n = resolved.size();
    char** result = new char*[static_cast<size_t>(n) + 1];
    for (int i = 0; i < n; ++i) {
        QByteArray utf8 = resolved[i].toUtf8();
        result[i] = new char[utf8.size() + 1];
        memcpy(result[i], utf8.constData(), static_cast<size_t>(utf8.size()) + 1);
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
    QString token = QtTokenReceiver::receiveAuthToken(QString::fromUtf8(plugin_name));
    std::string s = token.toStdString();
    char* result = new char[s.size() + 1];
    memcpy(result, s.c_str(), s.size() + 1);
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
