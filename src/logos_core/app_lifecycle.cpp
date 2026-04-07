#include "app_lifecycle.h"
#include "app_context.h"
#include "plugin_manager.h"
#include <cstdlib>
#include <random>

namespace {

    void ensureLogosInstanceId()
    {
        if (const char* existing = std::getenv("LOGOS_INSTANCE_ID")) {
            if (existing[0])
                return;
        }
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dis(0, 15);
        const char* hex = "0123456789abcdef";
        char id[13];
        for (int i = 0; i < 12; ++i)
            id[i] = hex[dis(gen)];
        id[12] = '\0';
        setenv("LOGOS_INSTANCE_ID", id, 1);
    }

}

namespace AppLifecycle {

    void init(int argc, char* argv[])
    {
        AppContext::init(argc, argv);
    }

    void start()
    {
        ensureLogosInstanceId();

        PluginManager::discoverInstalledModules();
        PluginManager::initializeCapabilityModule();
    }

    int exec()
    {
        return AppContext::exec();
    }

    void cleanup()
    {
        PluginManager::clear();
        AppContext::cleanup();
    }

    void processEvents()
    {
        AppContext::processEvents();
    }

}
