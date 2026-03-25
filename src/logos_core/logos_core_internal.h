#ifndef LOGOS_CORE_INTERNAL_H
#define LOGOS_CORE_INTERNAL_H

#include <QCoreApplication>
#include <QStringList>
#include <QHash>
#include <QList>
#include <QPair>
#include <QJsonObject>

#include <QProcess>

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
extern QHash<QString, QProcess*> g_plugin_processes;

#endif // LOGOS_CORE_INTERNAL_H
