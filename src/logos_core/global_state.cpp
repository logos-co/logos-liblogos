#include "logos_core_internal.h"
#include <QCoreApplication>
#include <QStringList>
#include <QHash>
#include <QList>
#include <QPair>
#include <QRemoteObjectRegistryHost>
#ifndef Q_OS_IOS
#include <QProcess>
#endif

// === Global State Definitions ===

// Global application pointer
QCoreApplication* g_app = nullptr;

// Flag to track if we created the app or are using an existing one
bool g_app_created_by_us = false;

// Custom plugins directories (supports multiple directories)
QStringList g_plugins_dirs;

// Global list to store loaded plugin names
QStringList g_loaded_plugins;

// Global hash to store known plugin names and paths
QHash<QString, QString> g_known_plugins;

// Global hash to store plugin processes
#ifndef Q_OS_IOS
QHash<QString, QProcess*> g_plugin_processes;
#endif

// Global hash to store LogosAPI instances for Local mode plugins
QHash<QString, LogosAPI*> g_local_plugin_apis;

// Global Qt Remote Object registry host
QRemoteObjectRegistryHost* g_registry_host = nullptr;

// Global list to store registered event listeners
QList<EventListener> g_event_listeners;

// Global hash to track previous CPU times for percentage calculation
QHash<qint64, QPair<double, qint64>> g_previous_cpu_times;
