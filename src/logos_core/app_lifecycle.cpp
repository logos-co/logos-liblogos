#include "app_lifecycle.h"
#include "plugin_manager.h"
#include "logos_instance.h"
#include <QCoreApplication>
#include <QMetaType>

Q_DECLARE_METATYPE(QObject*)

namespace {
    QCoreApplication* s_app = nullptr;
    bool s_app_created_by_us = false;
}

namespace AppLifecycle {

    void init(int argc, char* argv[]) {
        if (QCoreApplication::instance()) {
            s_app = QCoreApplication::instance();
            s_app_created_by_us = false;
        } else {
            s_app = new QCoreApplication(argc, argv);
            s_app_created_by_us = true;
        }

        qRegisterMetaType<QObject*>("QObject*");
    }

    void start() {
        LogosInstance::id();

        PluginManager::discoverPlugins();
        PluginManager::initializeCapabilityModule();
    }

    int exec() {
        if (!s_app) return -1;
        return s_app->exec();
    }

    void cleanup() {
        PluginManager::terminateAll();

        if (s_app_created_by_us) {
            delete s_app;
        }
        s_app = nullptr;
        s_app_created_by_us = false;
    }

    void processEvents() {
        if (!s_app) return;
        s_app->processEvents();
    }

    QCoreApplication* app() {
        return s_app;
    }

    bool isAppOwnedByUs() {
        return s_app_created_by_us;
    }

}
