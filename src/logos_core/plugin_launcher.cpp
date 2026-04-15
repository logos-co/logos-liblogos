#include "plugin_launcher.h"
#include "process_manager.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <cstdlib>
#include <boost/dll/runtime_symbol_info.hpp>

namespace fs = std::filesystem;

namespace {

    QString resolveLogosHostPath(const QStringList& pluginsDirs) {
        QString logosHostPath;

        const char* envPath = std::getenv("LOGOS_HOST_PATH");
        if (envPath) {
            logosHostPath = QString::fromUtf8(envPath);
        }

        if (logosHostPath.isEmpty()) {
            auto appDir = boost::dll::program_location().parent_path();
            auto normalized = (appDir / "logos_host").lexically_normal();
            logosHostPath = QString::fromStdString(normalized.string());
        }

        if (!fs::exists(logosHostPath.toStdString())) {
            if (!pluginsDirs.isEmpty()) {
                auto candidatePath = fs::absolute(
                    fs::path(pluginsDirs.first().toStdString()) / ".." / "bin" / "logos_host"
                ).lexically_normal();
                if (fs::exists(candidatePath)) {
                    logosHostPath = QString::fromStdString(candidatePath.string());
                }
            }
        }

        if (!fs::exists(logosHostPath.toStdString())) {
            spdlog::critical("logos_host not found at: {} - set LOGOS_HOST_PATH or place it next to the executable",
                             logosHostPath.toStdString());
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
                spdlog::critical("Plugin process crashed: {}", pName);
                exit(1);
            }
            if (onTerminated)
                onTerminated(qName);
        };

        callbacks.onError = [](const std::string& pName, bool crashed) {
            if (crashed) {
                spdlog::critical("Plugin process crashed: {}", pName);
                exit(1);
            }
        };

        callbacks.onOutput = [](const std::string& pName, const std::string& line, bool isStderr) {
            QString qLine = QString::fromStdString(line);
            if (isStderr) {
                spdlog::critical("[{}] {}", pName, line);
            } else if (qLine.contains("Warning:") || qLine.contains("WARNING:")) {
                spdlog::warn("[{}] {}", pName, line);
            } else if (qLine.contains("Critical:") || qLine.contains("FAILED:") || qLine.contains("ERROR:")) {
                spdlog::critical("[{}] {}", pName, line);
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
