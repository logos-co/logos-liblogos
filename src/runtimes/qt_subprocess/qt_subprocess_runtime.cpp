#include "qt_subprocess_runtime.h"
#include "subprocess_manager.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <cstdlib>
#include <boost/dll/runtime_symbol_info.hpp>

namespace fs = std::filesystem;

namespace {

std::string resolveLogosHostPath(const std::vector<std::string>& pluginsDirs) {
    std::string logosHostPath;

    const char* envPath = std::getenv("LOGOS_HOST_PATH");
    if (envPath)
        logosHostPath = envPath;

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
            if (fs::exists(candidatePath))
                logosHostPath = candidatePath.string();
        }
    }

    if (!fs::exists(logosHostPath)) {
        spdlog::critical(
            "logos_host not found at: {} - set LOGOS_HOST_PATH or place it next to the executable",
            logosHostPath);
        return {};
    }

    return logosHostPath;
}

} // anonymous namespace

namespace LogosCore {

bool QtSubprocessRuntime::canHandle(const ModuleDescriptor& desc) const
{
    return desc.format == "qt-plugin" || desc.format.empty();
}

bool QtSubprocessRuntime::load(const ModuleDescriptor& desc,
                                std::function<void(const std::string&)> onTerminated,
                                LoadedModuleHandle& out)
{
    std::string logosHostPath = resolveLogosHostPath(desc.pluginsDirs);
    if (logosHostPath.empty())
        return false;

    std::vector<std::string> arguments = {
        "--name", desc.name,
        "--path", desc.path
    };

    if (!desc.instancePersistencePath.empty()) {
        arguments.push_back("--instance-persistence-path");
        arguments.push_back(desc.instancePersistencePath);
    }

    SubprocessManager::ProcessCallbacks callbacks;

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

    if (!SubprocessManager::startProcess(desc.name, logosHostPath, arguments, callbacks))
        return false;

    out.name     = desc.name;
    out.pid      = SubprocessManager::getProcessId(desc.name);
    out.endpoint = "qtro+unix://" + desc.name;
    return true;
}

bool QtSubprocessRuntime::sendToken(const std::string& name, const std::string& token)
{
    return SubprocessManager::sendToken(name, token);
}

void QtSubprocessRuntime::terminate(const std::string& name)
{
    SubprocessManager::terminateProcess(name);
}

void QtSubprocessRuntime::terminateAll()
{
    SubprocessManager::terminateAll();
}

bool QtSubprocessRuntime::hasModule(const std::string& name) const
{
    return SubprocessManager::hasProcess(name);
}

std::optional<int64_t> QtSubprocessRuntime::pid(const std::string& name) const
{
    int64_t p = SubprocessManager::getProcessId(name);
    if (p < 0) return std::nullopt;
    return p;
}

std::unordered_map<std::string, int64_t> QtSubprocessRuntime::getAllPids() const
{
    return SubprocessManager::getAllProcessIds();
}

} // namespace LogosCore
