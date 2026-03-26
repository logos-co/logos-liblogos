#include "app_lifecycle.h"
#include "plugin_manager.h"
#include "logos_instance.h"
#include <QCoreApplication>
#include <QMetaType>

Q_DECLARE_METATYPE(QObject*)

namespace {
    QCoreApplication* s_app = nullptr;
    QCoreApplication* s_owned_app = nullptr;
}

namespace AppLifecycle {

    void init(int argc, char* argv[]) {
        if (QCoreApplication::instance()) {
            s_app = QCoreApplication::instance();
        } else {
            s_app = new QCoreApplication(argc, argv);
            s_owned_app = s_app;
        }

        qRegisterMetaType<QObject*>("QObject*");
    }

    void start() {
        LogosInstance::id();

        PluginManager::discoverInstalledModules();
        PluginManager::initializeCapabilityModule();
    }

    int exec() {
        if (!s_app) return -1;
        return s_app->exec();
    }

    void cleanup() {
        PluginManager::terminateAll();

        delete s_owned_app;
        s_owned_app = nullptr;
        s_app = nullptr;
    }

    void processEvents() {
        if (!s_app) return;
        s_app->processEvents();
    }

}
