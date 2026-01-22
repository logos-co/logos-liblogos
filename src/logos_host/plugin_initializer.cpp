#include "plugin_initializer.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QPluginLoader>
#include <QObject>
#include <QDebug>
#include "../common/interface.h"
#include "logos_api.h"
#include "logos_api_provider.h"
#include "token_manager.h"

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

    qDebug() << "Token server started, waiting for auth token...";

    // Wait for connection and receive token
    QString authToken;
    if (tokenServer->waitForNewConnection(10000)) { // Wait up to 10 seconds
        QLocalSocket* clientSocket = tokenServer->nextPendingConnection();
        if (clientSocket->waitForReadyRead(5000)) {
            QByteArray tokenData = clientSocket->readAll();
            authToken = QString::fromUtf8(tokenData);
            qDebug() << "Auth token received securely";
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

PluginInterface* loadPlugin(const QString& pluginPath, const QString& expectedName, QPluginLoader& loader)
{
    // Load the plugin
    loader.setFileName(pluginPath);
    QObject *plugin = loader.instance();

    if (!plugin) {
        qCritical() << "Failed to load plugin:" << loader.errorString();
        return nullptr;
    }

    qDebug() << "Plugin loaded successfully";

    // Cast to the base PluginInterface
    PluginInterface *basePlugin = qobject_cast<PluginInterface *>(plugin);
    if (!basePlugin) {
        qCritical() << "Plugin does not implement the PluginInterface";
        delete plugin;
        return nullptr;
    }

    // Verify that the plugin name matches
    if (expectedName != basePlugin->name()) {
        qWarning() << "Plugin name mismatch! Expected:" << expectedName << "Actual:" << basePlugin->name();
    }

    qDebug() << "Plugin name:" << basePlugin->name();
    qDebug() << "Plugin version:" << basePlugin->version();

    return basePlugin;
}

LogosAPI* initializeLogosAPI(const QString& pluginName, QObject* plugin, 
                              PluginInterface* basePlugin, const QString& authToken)
{
    // Initialize LogosAPI for this plugin
    LogosAPI* logos_api = new LogosAPI(pluginName, plugin);

    if (!logos_api) {
        qCritical() << "Failed to create LogosAPI instance";
        return nullptr;
    }

    qDebug() << "LogosAPI initialized for plugin:" << pluginName;

    // Register the plugin for remote access using LogosAPI Provider
    bool success = logos_api->getProvider()->registerObject(basePlugin->name(), plugin);
    if (success) {
        qDebug() << "Plugin registered for remote access with name:" << basePlugin->name();
        // Save the auth token using the TokenManager
        logos_api->getTokenManager()->saveToken("core", authToken);
        logos_api->getTokenManager()->saveToken("core_manager", authToken);
        logos_api->getTokenManager()->saveToken("capability_module", authToken);
        qDebug() << "Auth token saved for core access";
    } else {
        qCritical() << "Failed to register plugin for remote access:" << basePlugin->name();
        delete plugin;
        delete logos_api;
        return nullptr;
    }

    return logos_api;
}

LogosAPI* setupPlugin(const QString& pluginName, const QString& pluginPath, QPluginLoader& loader)
{
    // 1. Receive auth token securely
    QString authToken = receiveAuthToken(pluginName);
    if (authToken.isEmpty()) {
        return nullptr;
    }

    // 2. Load and validate plugin
    PluginInterface* basePlugin = loadPlugin(pluginPath, pluginName, loader);
    if (!basePlugin) {
        return nullptr;
    }

    // 3. Initialize LogosAPI and register plugin
    LogosAPI* logos_api = initializeLogosAPI(pluginName, loader.instance(), 
                                              basePlugin, authToken);
    return logos_api;
}
