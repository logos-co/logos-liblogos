#ifndef QT_PROCESS_MANAGER_H
#define QT_PROCESS_MANAGER_H

#include <QString>
#include <QStringList>
#include <QHash>
#include <functional>

namespace QtProcessManager {

    struct ProcessCallbacks {
        std::function<void(const QString& name, int exitCode, bool crashed)> onFinished;
        std::function<void(const QString& name, bool crashed)> onError;
        std::function<void(const QString& name, const QString& line, bool isStderr)> onOutput;
    };

    bool startProcess(const QString& name, const QString& executable,
                      const QStringList& arguments, const ProcessCallbacks& callbacks);
    bool sendToken(const QString& name, const QString& token);
    void terminateProcess(const QString& name);
    void terminateAll();
    bool hasProcess(const QString& name);
    qint64 getProcessId(const QString& name);
    QHash<QString, qint64> getAllProcessIds();
    void clearAll();
    void registerProcess(const QString& name);
}

#endif // QT_PROCESS_MANAGER_H
