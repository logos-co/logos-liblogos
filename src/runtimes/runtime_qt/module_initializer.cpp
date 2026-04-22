#include "module_initializer.h"
#include "qt/qt_token_receiver.h"
#include <QObject>
#include <spdlog/spdlog.h>
#include <filesystem>
#include "interface.h"
#include "logos_api.h"
#include "logos_api_provider.h"
#include "token_manager.h"
#include "module_lib.h"

namespace fs = std::filesystem;

using namespace ModuleLib;

LogosModule loadModule(const std::string& modulePath, const std::string& expectedName)
{
    std::string errorString;
    LogosModule module = LogosModule::loadFromPath(modulePath, &errorString);

    if (!module.isValid()) {
        spdlog::critical("Failed to load module: {}", errorString);
        return LogosModule();
    }

    PluginInterface *basePlugin = module.as<PluginInterface>();
    if (!basePlugin) {
        spdlog::critical("Module does not implement the PluginInterface");
        return LogosModule();
    }

    if (expectedName != basePlugin->name().toStdString()) {
        spdlog::warn("Module name mismatch: expected {} got {}",
                     expectedName, basePlugin->name().toStdString());
    }

    return module;
}

LogosAPI* initializeLogosAPI(const std::string& moduleName, QObject* module,
                              PluginInterface* basePlugin, const std::string& authToken,
                              const std::string& modulePath,
                              const std::string& instancePersistencePath)
{
    LogosAPI* logos_api = new LogosAPI(moduleName, module);
    logos_api->setProperty("modulePath",
        fs::absolute(fs::path(modulePath)).parent_path().string());

    if (!instancePersistencePath.empty()) {
        logos_api->setProperty("instancePersistencePath", instancePersistencePath);
        logos_api->setProperty("instanceId",
            fs::path(instancePersistencePath).filename().string());
    }

    bool success = logos_api->getProvider()->registerObject(basePlugin->name(), module);
    if (success) {
        logos_api->getTokenManager()->saveToken(std::string("core"), authToken);
        logos_api->getTokenManager()->saveToken(std::string("capability_module"), authToken);
    } else {
        spdlog::critical("Failed to register module for remote access: {}", basePlugin->name().toStdString());
        delete module;
        delete logos_api;
        return nullptr;
    }

    return logos_api;
}

LogosAPI* setupModule(const std::string& moduleName, const std::string& modulePath,
                      const std::string& instancePersistencePath)
{
    // 1. Receive auth token securely
    std::string authToken = QtTokenReceiver::receiveAuthToken(moduleName);
    if (authToken.empty()) {
        return nullptr;
    }

    // 2. Load and validate module
    LogosModule module = loadModule(modulePath, moduleName);
    if (!module.isValid()) {
        return nullptr;
    }

    // 3. Initialize LogosAPI and register module
    PluginInterface* basePlugin = module.as<PluginInterface>();
    LogosAPI* logos_api = initializeLogosAPI(moduleName, module.instance(),
                                              basePlugin, authToken, modulePath,
                                              instancePersistencePath);

    // Release module ownership so the module stays loaded
    module.release();

    return logos_api;
}
