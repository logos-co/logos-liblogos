#include "app_lifecycle.h"
#include "plugin_manager.h"
#include "logos_instance.h"
#include "qt/qt_app_context.h"

namespace AppLifecycle {

    void init(int argc, char* argv[]) {
        QtAppContext::init(argc, argv);
    }

    void start() {
        LogosInstance::id();

        PluginManager::discoverInstalledModules();
        PluginManager::initializeCapabilityModule();
    }

    int exec() {
        return QtAppContext::exec();
    }

    void cleanup() {
        PluginManager::terminateAll();
        QtAppContext::cleanup();
    }

    void processEvents() {
        QtAppContext::processEvents();
    }

}
