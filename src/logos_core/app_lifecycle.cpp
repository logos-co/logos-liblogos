#include "app_lifecycle.h"
#include "logos_core_internal.h"
#include "plugin_manager.h"
#include "logos_instance.h"
#include <QCoreApplication>
#include <QDebug>
#include <QMetaType>

Q_DECLARE_METATYPE(QObject*)

namespace AppLifecycle {

    void init(int argc, char* argv[]) {
        if (QCoreApplication::instance()) {
            g_app = QCoreApplication::instance();
            g_app_created_by_us = false;
            qDebug() << "Using existing QCoreApplication instance";
        } else {
            g_app = new QCoreApplication(argc, argv);
            g_app_created_by_us = true;
            qDebug() << "Created new QCoreApplication instance";
        }

        qRegisterMetaType<QObject*>("QObject*");
    }

    void start() {
        // Generate the shared instance ID before launching any child processes
        // so that logos_host processes inherit it via LOGOS_INSTANCE_ID env var.
        LogosInstance::id();

        PluginManager::discoverPlugins();
        PluginManager::initializeCapabilityModule();
    }

    int exec() {
        if (!g_app) return -1;
        return g_app->exec();
    }

    void cleanup() {
        PluginManager::terminateAll();

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

}
