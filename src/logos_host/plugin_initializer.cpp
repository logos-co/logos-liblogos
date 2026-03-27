#include "plugin_initializer.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QDebug>
#include <QFileInfo>
#include "interface.h"
#include "logos_api.h"
#include "logos_api_provider.h"
#include "token_manager.h"
#include "module_lib.h"

using namespace ModuleLib;

QString receiveAuthToken(const QString& pluginName)
{
    // Set up IPC server to receive auth token securely
    QString socketName = QString("logos_token_%1").arg(pluginName);
    QLocalServer* tokenServer = new QLocalServer();

    // Remove any existing socket file
    QLocalServer::removeServer(socketName);

    if (!tokenServer->listen(socketName)) {
        qCritical() << "Failed to start token server:" << tokenServer->errorString();
        return QString();
    }

    QString authToken;
    if (tokenServer->waitForNewConnection(10000)) {
        QLocalSocket* clientSocket = tokenServer->nextPendingConnection();
        if (clientSocket->waitForReadyRead(5000)) {
            QByteArray tokenData = clientSocket->readAll();
            authToken = QString::fromUtf8(tokenData);
        }
        clientSocket->deleteLater();
    } else {
        qCritical() << "Timeout waiting for auth token";
        tokenServer->deleteLater();
        return QString();
    }

    tokenServer->deleteLater();

    if (authToken.isEmpty()) {
        qCritical() << "No auth token received";
        return QString();
    }

    return authToken;
}

LogosModule loadPlugin(const QString& pluginPath, const QString& expectedName)
{
    // Load the plugin using module_lib for abstraction
    QString errorString;
    LogosModule module = LogosModule::loadFromPath(pluginPath, &errorString);

    if (!module.isValid()) {
        qCritical() << "Failed to load plugin:" << errorString;
        return LogosModule();
    }

    PluginInterface *basePlugin = module.as<PluginInterface>();
    if (!basePlugin) {
        qCritical() << "Plugin does not implement the PluginInterface";
        return LogosModule();
    }

    if (expectedName != basePlugin->name()) {
        qWarning() << "Plugin name mismatch: expected" << expectedName << "got" << basePlugin->name();
    }

    return module;
}

LogosAPI* initializeLogosAPI(const QString& pluginName, QObject* plugin, 
                              PluginInterface* basePlugin, const QString& authToken,
                              const QString& pluginPath)
{
    LogosAPI* logos_api = new LogosAPI(pluginName, plugin);
    logos_api->setProperty("modulePath", QFileInfo(pluginPath).absolutePath());

    bool success = logos_api->getProvider()->registerObject(basePlugin->name(), plugin);
    if (success) {
        logos_api->getTokenManager()->saveToken("core", authToken);
        logos_api->getTokenManager()->saveToken("capability_module", authToken);
    } else {
        qCritical() << "Failed to register plugin for remote access:" << basePlugin->name();
        delete plugin;
        delete logos_api;
        return nullptr;
    }

    return logos_api;
}

LogosAPI* setupPlugin(const QString& pluginName, const QString& pluginPath)
{
    // 1. Receive auth token securely
    QString authToken = receiveAuthToken(pluginName);
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
                                              basePlugin, authToken, pluginPath);
    
    // Release module ownership so the plugin stays loaded
    module.release();
    
    return logos_api;
}
