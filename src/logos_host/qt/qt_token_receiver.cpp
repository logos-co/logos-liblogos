#include "qt_token_receiver.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QString>
#include <spdlog/spdlog.h>

namespace QtTokenReceiver {

    std::string receiveAuthToken(const std::string& pluginName)
    {
        QString socketName = QString("logos_token_%1").arg(QString::fromStdString(pluginName));
        QLocalServer* tokenServer = new QLocalServer();

        QLocalServer::removeServer(socketName);

        if (!tokenServer->listen(socketName)) {
            spdlog::critical("Failed to start token server: {}", tokenServer->errorString().toStdString());
            return {};
        }

        std::string authToken;
        if (tokenServer->waitForNewConnection(10000)) {
            QLocalSocket* clientSocket = tokenServer->nextPendingConnection();
            if (clientSocket->waitForReadyRead(5000)) {
                QByteArray tokenData = clientSocket->readAll();
                authToken = QString::fromUtf8(tokenData).toStdString();
            }
            clientSocket->deleteLater();
        } else {
            spdlog::critical("Timeout waiting for auth token");
            tokenServer->deleteLater();
            return {};
        }

        tokenServer->deleteLater();

        if (authToken.empty()) {
            spdlog::critical("No auth token received");
            return {};
        }

        return authToken;
    }

}
