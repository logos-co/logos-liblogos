#include "app_lifecycle.h"
#include "logos_core_internal.h"
#include "plugin_manager.h"
#include "logos_mode.h"
#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QRemoteObjectRegistryHost>
#include <QMetaType>
#include <cassert>
#ifndef Q_OS_IOS
#include <QProcess>
#endif

// Declare QObject* as a metatype so it can be stored in QVariant
Q_DECLARE_METATYPE(QObject*)

namespace AppLifecycle {

    void init(int argc, char* argv[]) {
        // Check if an application instance already exists (e.g., when used in a Qt Quick app)
        if (QCoreApplication::instance()) {
            g_app = QCoreApplication::instance();
            g_app_created_by_us = false;
            qDebug() << "Using existing QCoreApplication instance";
        } else {
            // Create a new application instance (standalone mode)
            g_app = new QCoreApplication(argc, argv);
            g_app_created_by_us = true;
            qDebug() << "Created new QCoreApplication instance";
        }
        
        // Register QObject* as a metatype
        qRegisterMetaType<QObject*>("QObject*");
    }

    void setMode(int mode) {
        if (mode == 1) {
            LogosModeConfig::setMode(LogosMode::Local);
            qDebug() << "Logos mode set to: Local (in-process)";
            return;
        }
        LogosModeConfig::setMode(LogosMode::Remote);
        qDebug() << "Logos mode set to: Remote (separate processes)";
    }

    void setPluginsDir(const char* plugins_dir) {
        assert(plugins_dir != nullptr);
        g_plugins_dirs.clear();
        g_plugins_dirs.append(QString(plugins_dir));
        qInfo() << "Custom plugins directory set to:" << g_plugins_dirs.first();
    }

    void addPluginsDir(const char* plugins_dir) {
        assert(plugins_dir != nullptr);
        QString dir = QString(plugins_dir);
        if (g_plugins_dirs.contains(dir)) return;
        g_plugins_dirs.append(dir);
        qDebug() << "Added plugins directory:" << dir;
    }

    void start() {
        // Clear the list of loaded plugins before loading new ones
        g_loaded_plugins.clear();
        
        // Initialize Qt Remote Object registry host
        if (!g_registry_host) {
            g_registry_host = new QRemoteObjectRegistryHost(QUrl(QStringLiteral("local:logos_core_manager")));
            qDebug() << "Qt Remote Object registry host initialized at: local:logos_core_manager";
        }
        
        // First initialize the core manager
        if (!PluginManager::initializeCoreManager()) {
            qWarning() << "Failed to initialize core manager, continuing with other modules...";
        }
        
        // Define the plugins directories to scan
        QStringList pluginsDirs;
        if (!g_plugins_dirs.isEmpty()) {
            // Use the custom plugins directories if set
            pluginsDirs = g_plugins_dirs;
        } else {
            // Use the default plugins directory
            pluginsDirs << QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../modules");
        }
        
        qDebug() << "Looking for modules in" << pluginsDirs.size() << "directories:" << pluginsDirs;
        
        // Find and process all plugins in all directories to populate g_known_plugins
        for (const QString& pluginsDir : pluginsDirs) {
            qDebug() << "Scanning directory:" << pluginsDir;
            QStringList pluginPaths = PluginManager::findPlugins(pluginsDir);
            
            if (pluginPaths.isEmpty()) {
                qDebug() << "No modules found in:" << pluginsDir;
            } else {
                qDebug() << "Found" << pluginPaths.size() << "modules in:" << pluginsDir;
                
                // Process each plugin to add to known plugins list
                for (const QString &pluginPath : pluginPaths) {
                    QString pluginName = PluginManager::processPlugin(pluginPath);
                    if (pluginName.isEmpty()) {
                        qWarning() << "Failed to process plugin (no metadata or invalid):" << pluginPath;
                    } else {
                        qDebug() << "Successfully processed plugin:" << pluginName;
                    }
                }
            }
        }
        
        qDebug() << "Total known plugins after processing:" << g_known_plugins.size();
        qDebug() << "Known plugin names:" << g_known_plugins.keys();

        // Initialize capability module if available (after plugin discovery)
        PluginManager::initializeCapabilityModule();
    }

    int exec() {
        // assert(g_app != nullptr);
        if (!g_app) return -1;
        return g_app->exec();
    }

    void cleanup() {
        // Clean up Local mode plugins
        if (!g_local_plugin_apis.isEmpty()) {
            qDebug() << "Cleaning up Local mode plugins...";
            for (auto it = g_local_plugin_apis.begin(); it != g_local_plugin_apis.end(); ++it) {
                QString pluginName = it.key();
                LogosAPI* logos_api = it.value();
                
                qDebug() << "Cleaning up Local mode plugin:" << pluginName;
                delete logos_api;
            }
            g_local_plugin_apis.clear();
            qDebug() << "Local mode plugins cleaned up";
        }

    #ifndef Q_OS_IOS
        // Terminate all plugin processes (Remote mode)
        if (!g_plugin_processes.isEmpty()) {
            qDebug() << "Terminating all plugin processes...";
            for (auto it = g_plugin_processes.begin(); it != g_plugin_processes.end(); ++it) {
                QProcess* process = it.value();
                QString pluginName = it.key();
                
                qDebug() << "Terminating plugin process:" << pluginName;
                process->terminate();
                
                if (!process->waitForFinished(3000)) {
                    qWarning() << "Process did not terminate gracefully, killing it:" << pluginName;
                    process->kill();
                    process->waitForFinished(1000);
                }
                
                delete process;
            }
            g_plugin_processes.clear();
        }
    #endif
        g_loaded_plugins.clear();
        
        // Clean up Qt Remote Object registry host
        if (g_registry_host) {
            delete g_registry_host;
            g_registry_host = nullptr;
            qDebug() << "Qt Remote Object registry host cleaned up";
        }
        
        // Only delete the app if we created it
        if (g_app_created_by_us) {
            delete g_app;
        }
        g_app = nullptr;
        g_app_created_by_us = false;
    }

    void processEvents() {
        if (!g_app) return;
        g_app->processEvents();
    }

    QStringList getPluginsDirs() {
        return g_plugins_dirs;
    }

    bool isInitialized() {
        return g_app != nullptr;
    }

    bool isAppOwnedByUs() {
        return g_app_created_by_us;
    }

    bool isRegistryHostInitialized() {
        return g_registry_host != nullptr;
    }

}
