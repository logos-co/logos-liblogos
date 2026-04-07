#include "plugin_initializer.h"
#include "logos_logging.h"
#include "token_receiver.h"
#include <QObject>
#include <QString>
#include <filesystem>
#include "interface.h"
#include "logos_api.h"
#include "logos_api_provider.h"
#include "token_manager.h"
#include "module_lib.h"

using namespace ModuleLib;

LogosModule loadPlugin(const std::string& pluginPath, const std::string& expectedName)
{
    QString errorString;
    // logos-module / SDK boundary: paths are QString
    LogosModule module = LogosModule::loadFromPath(QString::fromStdString(pluginPath), &errorString);

    if (!module.isValid()) {
        logos_log_critical("Failed to load plugin: {}", errorString.toStdString());
        return LogosModule();
    }

    PluginInterface* basePlugin = module.as<PluginInterface>();
    if (!basePlugin) {
        logos_log_critical("Plugin does not implement the PluginInterface");
        return LogosModule();
    }

    if (expectedName != basePlugin->name().toStdString()) {
        logos_log_warn("Plugin name mismatch: expected {} got {}", expectedName, basePlugin->name().toStdString());
    }

    return module;
}

LogosAPI* initializeLogosAPI(const std::string& pluginName, QObject* plugin,
                             PluginInterface* basePlugin, const std::string& authToken,
                             const std::string& pluginPath)
{
    // logos-cpp-sdk boundary: LogosAPI and QObject use Qt types
    LogosAPI* logos_api = new LogosAPI(QString::fromStdString(pluginName), plugin);
    const std::filesystem::path absParent =
        std::filesystem::absolute(std::filesystem::path(pluginPath)).parent_path();
    logos_api->setProperty("modulePath", QString::fromStdString(absParent.string()));

    bool success = logos_api->getProvider()->registerObject(basePlugin->name(), plugin);
    if (success) {
        logos_api->getTokenManager()->saveToken("core", QString::fromStdString(authToken));
        logos_api->getTokenManager()->saveToken("capability_module", QString::fromStdString(authToken));
    } else {
        logos_log_critical("Failed to register plugin for remote access: {}", basePlugin->name().toStdString());
        delete plugin;
        delete logos_api;
        return nullptr;
    }

    return logos_api;
}

LogosAPI* setupPlugin(const std::string& pluginName, const std::string& pluginPath)
{
    std::string authToken = TokenReceiver::receiveAuthToken(pluginName);
    if (authToken.empty())
        return nullptr;

    LogosModule module = loadPlugin(pluginPath, pluginName);
    if (!module.isValid())
        return nullptr;

    PluginInterface* basePlugin = module.as<PluginInterface>();
    LogosAPI* logos_api =
        initializeLogosAPI(pluginName, module.instance(), basePlugin, authToken, pluginPath);

    module.release();

    return logos_api;
}
