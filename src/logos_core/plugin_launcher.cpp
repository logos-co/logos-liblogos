#include "plugin_launcher.h"
#include "executable_path.h"
#include "logos_logging.h"
#include "process_manager.h"
#include <cstdlib>
#include <filesystem>

namespace {

    std::filesystem::path resolveLogosHostPath(const std::vector<std::string>& pluginsDirs)
    {
        if (const char* envPath = std::getenv("LOGOS_HOST_PATH")) {
            if (envPath[0])
                return std::filesystem::path(envPath);
        }

        std::filesystem::path logosHostPath = logos_executable_directory() / "logos_host";

        if (!std::filesystem::exists(logosHostPath)) {
            if (!pluginsDirs.empty()) {
                std::filesystem::path candidate =
                    std::filesystem::weakly_canonical(std::filesystem::path(pluginsDirs.front()) / ".." / "bin" / "logos_host");
                if (std::filesystem::exists(candidate))
                    logosHostPath = candidate;
            }
        }

        if (!std::filesystem::exists(logosHostPath)) {
            logos_log_critical("logos_host not found at {} — set LOGOS_HOST_PATH or place it next to the executable",
                            logosHostPath.string());
            return {};
        }

        return logosHostPath;
    }

}

namespace PluginLauncher {

    bool launch(const std::string& name, const std::string& pluginPath,
                const std::vector<std::string>& pluginsDirs, OnTerminatedFn onTerminated)
    {
        const auto logosHostPath = resolveLogosHostPath(pluginsDirs);
        if (logosHostPath.empty())
            return false;

        const std::string hostExe = logosHostPath.string();
        std::vector<std::string> arguments = {"--name", name, "--path", pluginPath};

        ProcessManager::ProcessCallbacks callbacks;
        callbacks.onFinished = [onTerminated](const std::string& pName, int exitCode, bool crashed) {
            (void)exitCode;
            if (crashed) {
                logos_log_critical("Plugin process crashed: {}", pName);
                std::exit(1);
            }
            if (onTerminated)
                onTerminated(pName);
        };
        callbacks.onError = [](const std::string& pName, bool crashed) {
            if (crashed) {
                logos_log_critical("Plugin process crashed: {}", pName);
                std::exit(1);
            }
        };
        callbacks.onOutput = [](const std::string& pName, const std::string& line, bool isStderr) {
            if (isStderr) {
                logos_log_critical("[{}] {}", pName, line);
            } else if (line.find("Warning:") != std::string::npos || line.find("WARNING:") != std::string::npos) {
                logos_log_warn("[{}] {}", pName, line);
            } else if (line.find("Critical:") != std::string::npos || line.find("FAILED:") != std::string::npos
                       || line.find("ERROR:") != std::string::npos) {
                logos_log_critical("[{}] {}", pName, line);
            }
        };

        return ProcessManager::startProcess(name, hostExe, arguments, callbacks);
    }

    bool sendToken(const std::string& name, const std::string& token)
    {
        return ProcessManager::sendToken(name, token);
    }

    void terminate(const std::string& name)
    {
        ProcessManager::terminateProcess(name);
    }

    void terminateAll()
    {
        ProcessManager::terminateAll();
    }

    bool hasProcess(const std::string& name)
    {
        return ProcessManager::hasProcess(name);
    }

    std::unordered_map<std::string, int64_t> getAllProcessIds()
    {
        return ProcessManager::getAllProcessIds();
    }

}
