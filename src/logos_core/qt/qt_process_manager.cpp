#include "qt_process_manager.h"
#include <QDebug>
#include <QProcess>
#include <QLocalSocket>
#include <QThread>
#include <QHash>

namespace {
    QHash<QString, QProcess*> s_processes;

    QString toQ(const std::string& s) { return QString::fromStdString(s); }
    std::string fromQ(const QString& s) { return s.toStdString(); }
}

namespace QtProcessManager {

    bool startProcess(const std::string& name, const std::string& executable,
                      const std::vector<std::string>& arguments, const ProcessCallbacks& callbacks) {
        QString qName = toQ(name);
        QProcess* process = new QProcess();
        process->setProcessChannelMode(QProcess::MergedChannels);

        QStringList qArgs;
        for (const auto& arg : arguments) {
            qArgs << toQ(arg);
        }

        qDebug() << "Starting process with arguments:" << qArgs;

        process->start(toQ(executable), qArgs);

        if (!process->waitForStarted(5000)) {
            qCritical() << "Failed to start process:" << process->errorString();
            delete process;
            return false;
        }

        qDebug() << "Process started successfully for:" << qName;
        qDebug() << "Process ID:" << process->processId();

        s_processes.insert(qName, process);

        QObject::connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                        [name, qName, callbacks](int exitCode, QProcess::ExitStatus exitStatus) {
                            bool crashed = (exitStatus == QProcess::CrashExit);
                            qDebug() << "Process finished:" << qName
                                    << "Exit code:" << exitCode
                                    << "Exit status:" << exitStatus;

                            s_processes.remove(qName);

                            if (callbacks.onFinished) {
                                callbacks.onFinished(name, exitCode, crashed);
                            }
                        });

        QObject::connect(process, &QProcess::errorOccurred,
                        [name, qName, callbacks](QProcess::ProcessError error) {
                            bool crashed = (error == QProcess::Crashed);
                            qCritical() << "Process error for" << qName << ":" << error;

                            if (callbacks.onError) {
                                callbacks.onError(name, crashed);
                            }
                        });

        QObject::connect(process, &QProcess::readyReadStandardOutput,
                        [name, process, callbacks]() {
                            QByteArray output = process->readAllStandardOutput();
                            if (!output.isEmpty()) {
                                QString logLine = QString::fromUtf8(output).trimmed();
                                QStringList lines = logLine.split('\n', Qt::SkipEmptyParts);
                                for (const QString &line : lines) {
                                    if (callbacks.onOutput) {
                                        callbacks.onOutput(name, fromQ(line), false);
                                    }
                                }
                            }
                        });

        QObject::connect(process, &QProcess::readyReadStandardError,
                        [name, process, callbacks]() {
                            QByteArray output = process->readAllStandardError();
                            if (!output.isEmpty()) {
                                QString logLine = QString::fromUtf8(output).trimmed();
                                QStringList lines = logLine.split('\n', Qt::SkipEmptyParts);
                                for (const QString &line : lines) {
                                    if (callbacks.onOutput) {
                                        callbacks.onOutput(name, fromQ(line), true);
                                    }
                                }
                            }
                        });

        return true;
    }

    bool sendToken(const std::string& name, const std::string& token) {
        QString qName = toQ(name);
        QString socketName = QString("logos_token_%1").arg(qName);
        QLocalSocket* tokenSocket = new QLocalSocket();

        bool connected = false;
        for (int i = 0; i < 10; ++i) {
            tokenSocket->connectToServer(socketName);
            if (tokenSocket->waitForConnected(100)) {
                connected = true;
                break;
            }
            QThread::msleep(100);
        }

        if (!connected) {
            qCritical() << "Failed to connect to token socket for:" << qName;
            tokenSocket->deleteLater();

            if (s_processes.contains(qName)) {
                s_processes.value(qName)->terminate();
                delete s_processes.take(qName);
            }
            return false;
        }

        QByteArray tokenData = toQ(token).toUtf8();
        tokenSocket->write(tokenData);
        tokenSocket->waitForBytesWritten(1000);
        tokenSocket->disconnectFromServer();
        tokenSocket->deleteLater();

        qDebug() << "Token sent to:" << qName;
        return true;
    }

    void terminateProcess(const std::string& name) {
        QString qName = toQ(name);
        if (!s_processes.contains(qName)) return;

        QProcess* process = s_processes.take(qName);
        if (!process) return;

        qDebug() << "Terminating process for:" << qName;
        process->terminate();

        if (!process->waitForFinished(5000)) {
            qWarning() << "Process did not terminate gracefully, killing it:" << qName;
            process->kill();
            process->waitForFinished(2000);
        }
    }

    void terminateAll() {
        if (s_processes.isEmpty()) return;

        qDebug() << "Terminating all processes...";
        for (auto it = s_processes.begin(); it != s_processes.end(); ++it) {
            QProcess* process = it.value();
            QString processName = it.key();

            qDebug() << "Terminating process:" << processName;
            process->terminate();

            if (!process->waitForFinished(3000)) {
                qWarning() << "Process did not terminate gracefully, killing it:" << processName;
                process->kill();
                process->waitForFinished(1000);
            }

            delete process;
        }
        s_processes.clear();
    }

    bool hasProcess(const std::string& name) {
        return s_processes.contains(toQ(name));
    }

    int64_t getProcessId(const std::string& name) {
        QString qName = toQ(name);
        if (!s_processes.contains(qName)) return -1;
        QProcess* process = s_processes.value(qName);
        return process ? process->processId() : -1;
    }

    std::unordered_map<std::string, int64_t> getAllProcessIds() {
        std::unordered_map<std::string, int64_t> result;
        for (auto it = s_processes.begin(); it != s_processes.end(); ++it) {
            if (it.value()) {
                result[fromQ(it.key())] = it.value()->processId();
            }
        }
        return result;
    }

    void clearAll() {
        for (auto it = s_processes.begin(); it != s_processes.end(); ++it) {
            QProcess* process = it.value();
            if (process) {
                process->terminate();
                process->waitForFinished(1000);
                delete process;
            }
        }
        s_processes.clear();
    }

    void registerProcess(const std::string& name) {
        QString qName = toQ(name);
        if (!s_processes.contains(qName)) {
            s_processes.insert(qName, nullptr);
        }
    }

}
