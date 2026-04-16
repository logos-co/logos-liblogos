#include "plugin_initializer.h"
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

LogosModule loadPlugin(const std::string& pluginPath, const std::string& expectedName)
{
    // Load the plugin using module_lib for abstraction
    std::string errorString;
    LogosModule module = LogosModule::loadFromPath(pluginPath, &errorString);

    if (!module.isValid()) {
        spdlog::critical("Failed to load plugin: {}", errorString);
        return LogosModule();
    }

    PluginInterface *basePlugin = module.as<PluginInterface>();
    if (!basePlugin) {
        spdlog::critical("Plugin does not implement the PluginInterface");
        return LogosModule();
    }

    if (expectedName != basePlugin->name().toStdString()) {
        spdlog::warn("Plugin name mismatch: expected {} got {}",
                     expectedName, basePlugin->name().toStdString());
    }

    return module;
}

LogosAPI* initializeLogosAPI(const std::string& pluginName, QObject* plugin,
                              PluginInterface* basePlugin, const std::string& authToken,
                              const std::string& pluginPath,
                              const std::string& instancePersistencePath)
{
    LogosAPI* logos_api = new LogosAPI(pluginName, plugin);
    logos_api->setProperty("modulePath",
        fs::absolute(fs::path(pluginPath)).parent_path().string());

    if (!instancePersistencePath.empty()) {
        logos_api->setProperty("instancePersistencePath", instancePersistencePath);
        logos_api->setProperty("instanceId",
            fs::path(instancePersistencePath).filename().string());
    }

    bool success = logos_api->getProvider()->registerObject(basePlugin->name(), plugin);
    if (success) {
        logos_api->getTokenManager()->saveToken(std::string("core"), authToken);
        logos_api->getTokenManager()->saveToken(std::string("capability_module"), authToken);
    } else {
        spdlog::critical("Failed to register plugin for remote access: {}", basePlugin->name().toStdString());
        delete plugin;
        delete logos_api;
        return nullptr;
    }

    return logos_api;
}

LogosAPI* setupPlugin(const std::string& pluginName, const std::string& pluginPath,
                      const std::string& instancePersistencePath)
{
    // 1. Receive auth token securely
    std::string authToken = QtTokenReceiver::receiveAuthToken(pluginName);
    if (authToken.empty()) {
        return nullptr;
    }

    // 2. Load and validate plugin
    LogosModule module = loadPlugin(pluginPath, pluginName);
    if (!module.isValid()) {
        return nullptr;
    }

    // 3. Initialize LogosAPI and register plugin
    PluginInterface* basePlugin = module.as<PluginInterface>();
    LogosAPI* logos_api = initializeLogosAPI(pluginName, module.instance(),
                                              basePlugin, authToken, pluginPath,
                                              instancePersistencePath);

    // Release module ownership so the plugin stays loaded
    module.release();

    return logos_api;
}
