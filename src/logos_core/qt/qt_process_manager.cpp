#include "qt_process_manager.h"
#include <QDebug>
#include <QProcess>
#include <QLocalSocket>
#include <QThread>

namespace {
    QHash<QString, QProcess*> s_processes;
}

namespace QtProcessManager {

    bool startProcess(const QString& name, const QString& executable,
                      const QStringList& arguments, const ProcessCallbacks& callbacks) {
        QProcess* process = new QProcess();
        process->setProcessChannelMode(QProcess::MergedChannels);

        qDebug() << "Starting process with arguments:" << arguments;

        process->start(executable, arguments);

        if (!process->waitForStarted(5000)) {
            qCritical() << "Failed to start process:" << process->errorString();
            delete process;
            return false;
        }

        qDebug() << "Process started successfully for:" << name;
        qDebug() << "Process ID:" << process->processId();

        s_processes.insert(name, process);

        QObject::connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                        [name, callbacks](int exitCode, QProcess::ExitStatus exitStatus) {
                            bool crashed = (exitStatus == QProcess::CrashExit);
                            qDebug() << "Process finished:" << name
                                    << "Exit code:" << exitCode
                                    << "Exit status:" << exitStatus;

                            s_processes.remove(name);

                            if (callbacks.onFinished) {
                                callbacks.onFinished(name, exitCode, crashed);
                            }
                        });

        QObject::connect(process, &QProcess::errorOccurred,
                        [name, callbacks](QProcess::ProcessError error) {
                            bool crashed = (error == QProcess::Crashed);
                            qCritical() << "Process error for" << name << ":" << error;

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
                                        callbacks.onOutput(name, line, false);
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
                                        callbacks.onOutput(name, line, true);
                                    }
                                }
                            }
                        });

        return true;
    }

    bool sendToken(const QString& name, const QString& token) {
        QString socketName = QString("logos_token_%1").arg(name);
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
            qCritical() << "Failed to connect to token socket for:" << name;
            tokenSocket->deleteLater();

            if (s_processes.contains(name)) {
                s_processes.value(name)->terminate();
                delete s_processes.take(name);
            }
            return false;
        }

        QByteArray tokenData = token.toUtf8();
        tokenSocket->write(tokenData);
        tokenSocket->waitForBytesWritten(1000);
        tokenSocket->disconnectFromServer();
        tokenSocket->deleteLater();

        qDebug() << "Token sent to:" << name;
        return true;
    }

    void terminateProcess(const QString& name) {
        if (!s_processes.contains(name)) return;

        QProcess* process = s_processes.take(name);
        if (!process) return;

        qDebug() << "Terminating process for:" << name;
        process->terminate();

        if (!process->waitForFinished(5000)) {
            qWarning() << "Process did not terminate gracefully, killing it:" << name;
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

    bool hasProcess(const QString& name) {
        return s_processes.contains(name);
    }

    qint64 getProcessId(const QString& name) {
        if (!s_processes.contains(name)) return -1;
        QProcess* process = s_processes.value(name);
        return process ? process->processId() : -1;
    }

    QHash<QString, qint64> getAllProcessIds() {
        QHash<QString, qint64> result;
        for (auto it = s_processes.begin(); it != s_processes.end(); ++it) {
            if (it.value()) {
                result.insert(it.key(), it.value()->processId());
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

    void registerProcess(const QString& name) {
        if (!s_processes.contains(name)) {
            s_processes.insert(name, nullptr);
        }
    }

}
