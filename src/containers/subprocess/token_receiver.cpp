#include "token_receiver.h"
#include <QByteArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QString>
#include <spdlog/spdlog.h>

namespace SubprocessTokenReceiver {

std::string receive(const std::string& moduleName)
{
    const QByteArray instanceId = qgetenv("LOGOS_INSTANCE_ID");
    QString socketName = instanceId.isEmpty()
        ? QString("logos_token_%1").arg(QString::fromStdString(moduleName))
        : QString("logos_token_%1_%2")
              .arg(QString::fromStdString(moduleName))
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

} // namespace SubprocessTokenReceiver
