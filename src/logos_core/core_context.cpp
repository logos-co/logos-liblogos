#include "logos_core_internal.h"
#include <QCoreApplication>
#include <QStringList>
#include <QHash>
#include <QList>
#include <QPair>
#include <QJsonObject>
#include <QProcess>

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
QHash<QString, QProcess*> g_plugin_processes;

// List to store registered event listeners
QList<EventListener> g_event_listeners;
