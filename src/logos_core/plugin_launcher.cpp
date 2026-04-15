#include "plugin_launcher.h"
#include "process_manager.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <cstdlib>
#include <boost/dll/runtime_symbol_info.hpp>

namespace fs = std::filesystem;

namespace {

    std::string resolveLogosHostPath(const std::vector<std::string>& pluginsDirs) {
        std::string logosHostPath;

        const char* envPath = std::getenv("LOGOS_HOST_PATH");
        if (envPath) {
            logosHostPath = envPath;
        }

        if (logosHostPath.empty()) {
            auto appDir = boost::dll::program_location().parent_path();
            auto normalized = (appDir / "logos_host").lexically_normal();
            logosHostPath = normalized.string();
        }

        if (!fs::exists(logosHostPath)) {
            if (!pluginsDirs.empty()) {
                auto candidatePath = fs::absolute(
                    fs::path(pluginsDirs.front()) / ".." / "bin" / "logos_host"
                ).lexically_normal();
                if (fs::exists(candidatePath)) {
                    logosHostPath = candidatePath.string();
                }
            }
        }

        if (!fs::exists(logosHostPath)) {
            spdlog::critical("logos_host not found at: {} - set LOGOS_HOST_PATH or place it next to the executable",
                             logosHostPath);
            return {};
        }

        return logosHostPath;
    }

}

namespace PluginLauncher {

    bool launch(const std::string& name, const std::string& pluginPath,
                const std::vector<std::string>& pluginsDirs,
                const std::string& instancePersistencePath,
                OnTerminatedFn onTerminated) {
        std::string logosHostPath = resolveLogosHostPath(pluginsDirs);
        if (logosHostPath.empty())
            return false;

        std::vector<std::string> arguments = {
            "--name", name,
            "--path", pluginPath
        };

        if (!instancePersistencePath.empty()) {
            arguments.push_back("--instance-persistence-path");
            arguments.push_back(instancePersistencePath);
        }

        QtProcessManager::ProcessCallbacks callbacks;

        callbacks.onFinished = [onTerminated](const std::string& pName, int exitCode, bool crashed) {
            (void)exitCode;
            if (crashed) {
                spdlog::critical("Plugin process crashed: {}", pName);
                exit(1);
            }
            if (onTerminated)
                onTerminated(pName);
        };

        callbacks.onError = [](const std::string& pName, bool crashed) {
            if (crashed) {
                spdlog::critical("Plugin process crashed: {}", pName);
                exit(1);
            }
        };

        callbacks.onOutput = [](const std::string& pName, const std::string& line, bool isStderr) {
            if (isStderr) {
                spdlog::critical("[{}] {}", pName, line);
            } else if (line.find("Warning:") != std::string::npos ||
                       line.find("WARNING:") != std::string::npos) {
                spdlog::warn("[{}] {}", pName, line);
            } else if (line.find("Critical:") != std::string::npos ||
                       line.find("FAILED:") != std::string::npos ||
                       line.find("ERROR:") != std::string::npos) {
                spdlog::critical("[{}] {}", pName, line);
            }
        };

        return QtProcessManager::startProcess(name, logosHostPath, arguments, callbacks);
    }

    bool sendToken(const std::string& name, const std::string& token) {
        return QtProcessManager::sendToken(name, token);
    }

    void terminate(const std::string& name) {
        QtProcessManager::terminateProcess(name);
    }

    void terminateAll() {
        QtProcessManager::terminateAll();
    }

    bool hasProcess(const std::string& name) {
        return QtProcessManager::hasProcess(name);
    }

    std::unordered_map<std::string, int64_t> getAllProcessIds() {
        return QtProcessManager::getAllProcessIds();
    }

}
