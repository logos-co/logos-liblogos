#include "logos_core_internal.h"
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

// === Core Context Definitions ===

// Application pointer
QCoreApplication* g_app = nullptr;

// Flag to track if we created the app or are using an existing one
bool g_app_created_by_us = false;

// Custom plugins directories (supports multiple directories)
QStringList g_plugins_dirs;

// List to store loaded plugin names
QStringList g_loaded_plugins;

// Hash to store known plugin names and paths
QHash<QString, QString> g_known_plugins;

// Hash to store plugin metadata (name -> metadata JSON object)
QHash<QString, QJsonObject> g_plugin_metadata;

// Hash to store plugin processes
#ifndef Q_OS_IOS
QHash<QString, QProcess*> g_plugin_processes;
#endif

// Hash to store LogosAPI instances for Local mode plugins
QHash<QString, LogosAPI*> g_local_plugin_apis;

// Qt Remote Object registry host
QRemoteObjectRegistryHost* g_registry_host = nullptr;

// List to store registered event listeners
QList<EventListener> g_event_listeners;

// Hash to track previous CPU times for percentage calculation
QHash<qint64, QPair<double, qint64>> g_previous_cpu_times;
