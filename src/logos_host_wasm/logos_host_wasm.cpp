#include <QString>
#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QDebug>
#include <CLI/CLI.hpp>
#include "logos_api.h"
#include "logos_api_provider.h"
#include "token_manager.h"
#include "wasm_provider_object.h"

struct PluginArgs {
    std::string name;
    std::string path;
    bool valid = false;
};

static PluginArgs parseArgs(int argc, char* argv[])
{
    PluginArgs result;
    CLI::App app{"Logos host for WebAssembly modules"};
    app.set_version_flag("-v,--version", "1.0");
    app.add_option("-n,--name", result.name, "Module name")->required();
    app.add_option("-p,--path", result.path, "Path to .wasm file")->required();
    try { app.parse(argc, argv); }
    catch (const CLI::ParseError& e) { app.exit(e); return result; }
    result.valid = true;
    return result;
}

// Inline token receiver (mirrors logos_host's QtTokenReceiver)
static QString receiveAuthToken(const QString& pluginName)
{
    QString socketName = QString("logos_token_%1").arg(pluginName);
    QLocalServer server;
    QLocalServer::removeServer(socketName);
    if (!server.listen(socketName)) {
        qCritical() << "Failed to start token server:" << server.errorString();
        return {};
    }
    qDebug() << "Token server started, waiting for auth token...";
    QString token;
    if (server.waitForNewConnection(10000)) {
        QLocalSocket* sock = server.nextPendingConnection();
        if (sock->waitForReadyRead(5000))
            token = QString::fromUtf8(sock->readAll());
        sock->deleteLater();
    } else {
        qCritical() << "Timeout waiting for auth token";
    }
    if (!token.isEmpty())
        qDebug() << "Auth token received securely";
    return token;
}

int main(int argc, char* argv[])
{
    PluginArgs args = parseArgs(argc, argv);
    if (!args.valid) return 1;

    QCoreApplication app(argc, argv);

    QString name = QString::fromStdString(args.name);
    QString path = QString::fromStdString(args.path);

    qDebug() << "Logos WASM host starting for module:" << name;
    qDebug() << "WASM path:" << path;

    // 1. Receive auth token
    QString authToken = receiveAuthToken(name);
    if (authToken.isEmpty()) return 1;

    // 2. Load the .wasm module
    auto* wasmProvider = new WasmProviderObject(path, name, "1.0.0");
    if (!wasmProvider->isValid()) {
        qCritical() << "Failed to load WASM module:" << path;
        delete wasmProvider;
        return 1;
    }

    qDebug() << "Plugin loaded successfully";
    qDebug() << "Plugin name:" << name;

    // 3. Create LogosAPI and register for IPC
    // LogosAPI parent must be a QObject; we use the app as a container
    auto* container = new QObject();
    LogosAPI* logosApi = new LogosAPI(name, container);

    bool success = logosApi->getProvider()->registerObject(name, wasmProvider);
    if (!success) {
        qCritical() << "Failed to register WASM module for remote access:" << name;
        delete container;
        delete wasmProvider;
        return 1;
    }

    // 4. Save auth tokens
    logosApi->getTokenManager()->saveToken("core", authToken);
    logosApi->getTokenManager()->saveToken("capability_module", authToken);

    qDebug() << "Auth token saved for core access";
    qDebug() << "Logos WASM host ready, entering event loop...";

    int result = app.exec();
    delete container;
    return result;
}
