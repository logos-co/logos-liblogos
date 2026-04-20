#include "plugin_launcher.h"
#include "process_manager.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <filesystem>
#include <cstdlib>
#include <boost/dll/runtime_symbol_info.hpp>

namespace fs = std::filesystem;

namespace {

    // Dedicated logger for child stdout — uses a pattern that prints a
    // fictional "[out]" level, so stdout lines visually align with spdlog
    // output but are clearly distinguished from stderr-classified lines.
    std::shared_ptr<spdlog::logger>& moduleStdoutLogger() {
        static std::shared_ptr<spdlog::logger> logger = []() {
            auto l = spdlog::stdout_color_mt("logos_module_stdout");
            l->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [out] %v");
            return l;
        }();
        return logger;
    }

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
            if (!isStderr) {
                moduleStdoutLogger()->info("[{}] {}", pName, line);
                return;
            }
            auto contains = [&](std::initializer_list<const char*> keywords) {
                for (const char* k : keywords)
                    if (line.find(k) != std::string::npos) return true;
                return false;
            };
            // stderr: map common level prefixes (Qt's default message handler,
            // test/assertion frameworks, spdlog-style output from children)
            // onto spdlog levels. Default to info when no prefix is recognised.
            if (contains({"Critical:", "CRITICAL:", "Fatal:", "FATAL:"}))
                spdlog::critical("[{}] {}", pName, line);
            else if (contains({"Error:", "ERROR:", "FAILED:"}))
                spdlog::error("[{}] {}", pName, line);
            else if (contains({"Warning:", "WARNING:"}))
                spdlog::warn("[{}] {}", pName, line);
            else if (contains({"Debug:", "DEBUG:"}))
                spdlog::debug("[{}] {}", pName, line);
            else if (contains({"Trace:", "TRACE:"}))
                spdlog::trace("[{}] {}", pName, line);
            else
                spdlog::info("[{}] {}", pName, line);
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
