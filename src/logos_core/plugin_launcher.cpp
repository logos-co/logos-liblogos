#include "plugin_launcher.h"
#include "qt/qt_process_manager.h"
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QCoreApplication>

namespace {

    QString resolveHostBinary(const QString& binaryName, const QString& envVar,
                              const QStringList& pluginsDirs) {
        QString hostPath;

        QByteArray envPathBytes = qgetenv(envVar.toUtf8().constData());
        if (!envPathBytes.isEmpty()) {
            hostPath = QString::fromUtf8(envPathBytes);
        }

        if (hostPath.isEmpty()) {
            hostPath = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/" + binaryName);
        }

        if (!QFile::exists(hostPath)) {
            if (!pluginsDirs.isEmpty()) {
                QDir pluginsDirCandidate(pluginsDirs.first());
                QString candidate = QDir::cleanPath(pluginsDirCandidate.absoluteFilePath("../bin/" + binaryName));
                if (QFile::exists(candidate)) {
                    hostPath = candidate;
                }
            }
        }

        return hostPath;
    }

    QString resolveLogosHostPath(const QStringList& pluginsDirs) {
        QString path = resolveHostBinary("logos_host", "LOGOS_HOST_PATH", pluginsDirs);
        if (!QFile::exists(path)) {
            qCritical() << "logos_host not found at:" << path
                         << "- set LOGOS_HOST_PATH or place it next to the executable";
            return QString();
        }
        return path;
    }

    QString resolveLogosHostWasmPath(const QStringList& pluginsDirs) {
        QString path = resolveHostBinary("logos_host_wasm", "LOGOS_HOST_WASM_PATH", pluginsDirs);
        if (!QFile::exists(path)) {
            qCritical() << "logos_host_wasm not found at:" << path
                         << "- set LOGOS_HOST_WASM_PATH or place it next to the executable";
            return QString();
        }
        return path;
    }

}

namespace PluginLauncher {

    bool launch(const QString& name, const QString& pluginPath,
                const QStringList& pluginsDirs, OnTerminatedFn onTerminated) {

        // Choose host binary based on module type
        bool isWasm = pluginPath.endsWith(".wasm", Qt::CaseInsensitive);
        QString hostPath = isWasm
            ? resolveLogosHostWasmPath(pluginsDirs)
            : resolveLogosHostPath(pluginsDirs);

        if (hostPath.isEmpty())
            return false;

        qDebug() << "Loading plugin:" << name << "from path:" << pluginPath
                 << "in separate process";
        qDebug() << "Logos host path (resolved):" << hostPath;

        std::vector<std::string> arguments = {
            "--name", name.toStdString(),
            "--path", pluginPath.toStdString()
        };

        QtProcessManager::ProcessCallbacks callbacks;

        callbacks.onFinished = [onTerminated](const std::string& pName, int exitCode, bool crashed) {
            Q_UNUSED(exitCode);
            QString qName = QString::fromStdString(pName);
            if (crashed) {
                qCritical() << "Plugin process crashed:" << qName
                            << "- terminating core with error";
                exit(1);
            }
            if (onTerminated)
                onTerminated(qName);
        };

        callbacks.onError = [](const std::string& pName, bool crashed) {
            if (crashed) {
                qCritical() << "Process error for" << QString::fromStdString(pName)
                            << ": QProcess::Crashed";
                exit(1);
            }
        };

        callbacks.onOutput = [](const std::string& pName, const std::string& line, bool isStderr) {
            QString qName = QString::fromStdString(pName);
            QString qLine = QString::fromStdString(line);
            if (isStderr) {
                qCritical() << "[" << qName << "]" << qLine;
            } else if (qLine.contains("Warning:") || qLine.contains("WARNING:")) {
                qWarning() << "[" << qName << "]" << qLine;
            } else if (qLine.contains("Critical:") || qLine.contains("FAILED:") || qLine.contains("ERROR:")) {
                qCritical() << "[" << qName << "]" << qLine;
            }
        };

        return QtProcessManager::startProcess(name.toStdString(), hostPath.toStdString(), arguments, callbacks);
    }

    bool sendToken(const QString& name, const QString& token) {
        return QtProcessManager::sendToken(name.toStdString(), token.toStdString());
    }

    void terminate(const QString& name) {
        QtProcessManager::terminateProcess(name.toStdString());
    }

    void terminateAll() {
        QtProcessManager::terminateAll();
    }

    bool hasProcess(const QString& name) {
        return QtProcessManager::hasProcess(name.toStdString());
    }

    QHash<QString, qint64> getAllProcessIds() {
        auto stdMap = QtProcessManager::getAllProcessIds();
        QHash<QString, qint64> result;
        for (const auto& [name, pid] : stdMap) {
            result.insert(QString::fromStdString(name), pid);
        }
        return result;
    }

}
