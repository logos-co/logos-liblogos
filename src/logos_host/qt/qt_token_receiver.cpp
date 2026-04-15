#include "qt_token_receiver.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <spdlog/spdlog.h>

namespace QtTokenReceiver {

    QString receiveAuthToken(const QString& pluginName)
    {
        QString socketName = QString("logos_token_%1").arg(pluginName);
        QLocalServer* tokenServer = new QLocalServer();

        QLocalServer::removeServer(socketName);

        if (!tokenServer->listen(socketName)) {
            spdlog::critical("Failed to start token server: {}", tokenServer->errorString().toStdString());
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
            spdlog::critical("Timeout waiting for auth token");
            tokenServer->deleteLater();
            return QString();
        }

        tokenServer->deleteLater();

        if (authToken.isEmpty()) {
            spdlog::critical("No auth token received");
            return QString();
        }

        return authToken;
    }

}
