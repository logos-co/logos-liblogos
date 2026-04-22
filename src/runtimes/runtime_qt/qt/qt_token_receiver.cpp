#include "qt_token_receiver.h"
#include <QByteArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QString>
#include <spdlog/spdlog.h>

namespace QtTokenReceiver {

    std::string receiveAuthToken(const std::string& pluginName)
    {
        // Scope the socket name by LOGOS_INSTANCE_ID so parallel Logos instances
        // (multiple daemons or Basecamp profiles) loading the same module don't
        // race on a shared socket. The sender in SubprocessManager uses the same
        // scheme; both read the instance ID from the inherited env var.
        const QByteArray instanceId = qgetenv("LOGOS_INSTANCE_ID");
        QString socketName = instanceId.isEmpty()
            ? QString("logos_token_%1").arg(QString::fromStdString(pluginName))
            : QString("logos_token_%1_%2")
                  .arg(QString::fromStdString(pluginName))
                  .arg(QString::fromUtf8(instanceId));
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
