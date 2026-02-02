#ifndef LOGOS_CORE_INTERNAL_H
#define LOGOS_CORE_INTERNAL_H

#include <QCoreApplication>
#include <QStringList>
#include <QHash>
#include <QList>
#include <QPair>
#include <QJsonObject>
#include <QRemoteObjectRegistryHost>

#ifndef Q_OS_IOS
#include <QProcess>
#endif

#include "logos_core.h"

// Forward declarations
class LogosAPI;

// Global application pointer
extern QCoreApplication* g_app;

// Flag to track if we created the app or are using an existing one
extern bool g_app_created_by_us;

// Custom plugins directories (supports multiple directories)
extern QStringList g_plugins_dirs;

// Global list to store loaded plugin names
extern QStringList g_loaded_plugins;

// Global hash to store known plugin names and paths
extern QHash<QString, QString> g_known_plugins;

// Global hash to store plugin metadata (name -> metadata JSON object)
extern QHash<QString, QJsonObject> g_plugin_metadata;

// Global hash to store plugin processes
#ifndef Q_OS_IOS
extern QHash<QString, QProcess*> g_plugin_processes;
#endif

// Global hash to store LogosAPI instances for Local mode plugins
extern QHash<QString, LogosAPI*> g_local_plugin_apis;

// Global Qt Remote Object registry host
extern QRemoteObjectRegistryHost* g_registry_host;

// Structure to store event listener information
struct EventListener {
    QString pluginName;
    QString eventName;
    AsyncCallback callback;
    void* userData;
};

// Global list to store registered event listeners
extern QList<EventListener> g_event_listeners;

#endif // LOGOS_CORE_INTERNAL_H
