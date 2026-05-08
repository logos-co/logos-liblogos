#include "token_receiver.h"
#include "unix_socket_path.h"
#include <QByteArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QString>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string>

namespace SubprocessTokenReceiver {

std::string receive(const std::string& moduleName)
{
    // Scope the socket name by LOGOS_INSTANCE_ID so parallel Logos instances
    // (multiple daemons or Basecamp profiles) loading the same module don't
    // race on a shared socket. SubprocessContainer (parent) uses the same
    // scheme; both read the instance ID from the inherited env var.
    const char* instanceIdEnv = std::getenv("LOGOS_INSTANCE_ID");
    std::string socketName = "logos_token_" + moduleName;
    if (instanceIdEnv && instanceIdEnv[0]) {
        socketName += '_';
        socketName += instanceIdEnv;
    }

    // Resolve the full absolute path using the shared Qt-free helper
    // (see unix_socket_path.h). The parent in SubprocessContainer uses
    // the same helper for connect(); passing the absolute path to
    // QLocalServer::listen() bypasses Qt's QDir::tempPath() resolution
    // so the two sides cannot disagree under environments like
    // `nix run`, where $TMPDIR is unset on macOS and naive
    // getenv("TMPDIR") falls back to /tmp/<name> while Qt resolves
    // /var/folders/.../T/<name> via confstr(_CS_DARWIN_USER_TEMP_DIR).
    const std::string socketPath = ::logos::unixSocketPath(socketName);
    const QString socketPathQ = QString::fromStdString(socketPath);

    // Stack-allocated: the Qt event loop is not guaranteed to be
    // running when this function exits, so deleteLater() is unsafe.
    // RAII makes every exit path (incl. listen() failure) clean up.
    QLocalServer tokenServer;

    QLocalServer::removeServer(socketPathQ);

    if (!tokenServer.listen(socketPathQ)) {
        spdlog::critical("Failed to start token server at {}: {}", socketPath,
                         tokenServer.errorString().toStdString());
        return {};
    }

    std::string authToken;
    if (tokenServer.waitForNewConnection(10000)) {
        QLocalSocket* clientSocket = tokenServer.nextPendingConnection();
        if (clientSocket->waitForReadyRead(5000)) {
            QByteArray tokenData = clientSocket->readAll();
            authToken = QString::fromUtf8(tokenData).toStdString();
        }
        // The socket is parented to tokenServer and dies with it.
        clientSocket->disconnectFromServer();
    } else {
        spdlog::critical("Timeout waiting for auth token");
        return {};
    }

    if (authToken.empty()) {
        spdlog::critical("No auth token received");
        return {};
    }

    return authToken;
}

} // namespace SubprocessTokenReceiver
