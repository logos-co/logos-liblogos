#include "plugin_initializer.h"
#include "qt/qt_token_receiver.h"
#include <QObject>
#include <spdlog/spdlog.h>
#include <QDir>
#include <QFileInfo>
#include "interface.h"
#include "logos_api.h"
#include "logos_api_provider.h"
#include "token_manager.h"
#include "module_lib.h"

using namespace ModuleLib;

LogosModule loadPlugin(const QString& pluginPath, const QString& expectedName)
{
    // Load the plugin using module_lib for abstraction
    QString errorString;
    LogosModule module = LogosModule::loadFromPath(pluginPath, &errorString);

    if (!module.isValid()) {
        spdlog::critical("Failed to load plugin: {}", errorString.toStdString());
        return LogosModule();
    }

    PluginInterface *basePlugin = module.as<PluginInterface>();
    if (!basePlugin) {
        spdlog::critical("Plugin does not implement the PluginInterface");
        return LogosModule();
    }

    if (expectedName != basePlugin->name()) {
        spdlog::warn("Plugin name mismatch: expected {} got {}",
                     expectedName.toStdString(), basePlugin->name().toStdString());
    }

    return module;
}

LogosAPI* initializeLogosAPI(const QString& pluginName, QObject* plugin,
                              PluginInterface* basePlugin, const QString& authToken,
                              const QString& pluginPath,
                              const QString& instancePersistencePath)
{
    LogosAPI* logos_api = new LogosAPI(pluginName, plugin);
    logos_api->setProperty("modulePath", QFileInfo(pluginPath).absolutePath());

    if (!instancePersistencePath.isEmpty()) {
        logos_api->setProperty("instancePersistencePath", instancePersistencePath);
        logos_api->setProperty("instanceId", QDir(instancePersistencePath).dirName());
    }

    bool success = logos_api->getProvider()->registerObject(basePlugin->name(), plugin);
    if (success) {
        logos_api->getTokenManager()->saveToken("core", authToken);
        logos_api->getTokenManager()->saveToken("capability_module", authToken);
    } else {
        spdlog::critical("Failed to register plugin for remote access: {}", basePlugin->name().toStdString());
        delete plugin;
        delete logos_api;
        return nullptr;
    }

    return logos_api;
}

LogosAPI* setupPlugin(const QString& pluginName, const QString& pluginPath,
                      const QString& instancePersistencePath)
{
    // 1. Receive auth token securely
    QString authToken = QtTokenReceiver::receiveAuthToken(pluginName);
    if (authToken.isEmpty()) {
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
