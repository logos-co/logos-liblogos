#include "plugin_launcher.h"
#include "qt/qt_process_manager.h"
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QCoreApplication>

namespace {

    QString resolveLogosHostPath(const QStringList& pluginsDirs) {
        QString logosHostPath;

        QByteArray envPathBytes = qgetenv("LOGOS_HOST_PATH");
        if (!envPathBytes.isEmpty()) {
            logosHostPath = QString::fromUtf8(envPathBytes);
        }

        if (logosHostPath.isEmpty()) {
            logosHostPath = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/logos_host");
        }

        if (!QFile::exists(logosHostPath)) {
            if (!pluginsDirs.isEmpty()) {
                QDir pluginsDirCandidate(pluginsDirs.first());
                QString candidate = QDir::cleanPath(pluginsDirCandidate.absoluteFilePath("../bin/logos_host"));
                if (QFile::exists(candidate)) {
                    logosHostPath = candidate;
                }
            }
        }

        if (!QFile::exists(logosHostPath)) {
            qCritical() << "logos_host not found at:" << logosHostPath
                         << "- set LOGOS_HOST_PATH or place it next to the executable";
            return QString();
        }

        return logosHostPath;
    }

}

namespace PluginLauncher {

    bool launch(const QString& name, const QString& pluginPath,
                const QStringList& pluginsDirs,
                const QString& instancePersistencePath,
                OnTerminatedFn onTerminated) {
        QString logosHostPath = resolveLogosHostPath(pluginsDirs);
        if (logosHostPath.isEmpty())
            return false;

        std::vector<std::string> arguments = {
            "--name", name.toStdString(),
            "--path", pluginPath.toStdString()
        };

        if (!instancePersistencePath.isEmpty()) {
            arguments.push_back("--instance-persistence-path");
            arguments.push_back(instancePersistencePath.toStdString());
        }

        QtProcessManager::ProcessCallbacks callbacks;

        callbacks.onFinished = [onTerminated](const std::string& pName, int exitCode, bool crashed) {
            Q_UNUSED(exitCode);
            QString qName = QString::fromStdString(pName);
            if (crashed) {
                qCritical() << "Plugin process crashed:" << qName;
                exit(1);
            }
            if (onTerminated)
                onTerminated(qName);
        };

        callbacks.onError = [](const std::string& pName, bool crashed) {
            if (crashed) {
                qCritical() << "Plugin process crashed:" << QString::fromStdString(pName);
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

        return QtProcessManager::startProcess(name.toStdString(), logosHostPath.toStdString(), arguments, callbacks);
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
