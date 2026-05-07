#include "sandbox_container.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string tmpDir() {
    const char* tmp = std::getenv("TMPDIR");
    std::string dir = (tmp && tmp[0]) ? tmp : "/tmp";
    while (!dir.empty() && dir.back() == '/') dir.pop_back();
    return dir;
}

std::string tokenSocketPath(const std::string& moduleName) {
    const char* instanceId = std::getenv("LOGOS_INSTANCE_ID");
    std::string socketName = "logos_token_" + moduleName;
    if (instanceId && *instanceId) {
        socketName += "_";
        socketName += instanceId;
    }
    return tmpDir() + "/" + socketName;
}

} // anonymous namespace

// ===========================================================================
// ModuleContainer interface
// ===========================================================================

std::string SandboxContainer::id() const { return "sandbox"; }

bool SandboxContainer::canHandle(const LogosCore::ModuleDescriptor& /*desc*/) const
{
    return false;
}

bool SandboxContainer::launch(const LogosCore::ModuleDescriptor& desc,
                               const std::string& hostBinary,
                               const std::vector<std::string>& args,
                               std::function<void(const std::string&)> onTerminated,
                               LogosCore::LoadedModuleHandle& out)
{
#if !defined(__APPLE__)
    (void)desc; (void)hostBinary; (void)args; (void)onTerminated; (void)out;
    spdlog::error("SandboxContainer is only supported on macOS");
    return false;
#else
    std::string profile = generateProfile(desc, hostBinary);

    std::string profilePath = writeProfileFile(desc.name, profile);
    if (profilePath.empty())
        return false;

    {
        std::lock_guard<std::mutex> lock(profilesMutex_);
        profilePaths_[desc.name] = profilePath;
    }

    std::string sandboxExec = "/usr/bin/sandbox-exec";
    std::vector<std::string> sandboxArgs = {"-f", profilePath, hostBinary};
    sandboxArgs.insert(sandboxArgs.end(), args.begin(), args.end());

    auto wrappedOnTerminated = [this, onTerminated](const std::string& name) {
        cleanupProfile(name);
        if (onTerminated)
            onTerminated(name);
    };

    return subprocess_.launch(desc, sandboxExec, sandboxArgs,
                              std::move(wrappedOnTerminated), out);
#endif
}

bool SandboxContainer::sendToken(const std::string& name, const std::string& token)
{
    return subprocess_.sendToken(name, token);
}

void SandboxContainer::terminate(const std::string& name)
{
    subprocess_.terminate(name);
    cleanupProfile(name);
}

void SandboxContainer::terminateAll()
{
    subprocess_.terminateAll();
    std::lock_guard<std::mutex> lock(profilesMutex_);
    for (auto& [name, path] : profilePaths_)
        std::remove(path.c_str());
    profilePaths_.clear();
}

bool SandboxContainer::hasModule(const std::string& name) const
{
    return subprocess_.hasModule(name);
}

std::optional<int64_t> SandboxContainer::pid(const std::string& name) const
{
    return subprocess_.pid(name);
}

std::unordered_map<std::string, int64_t> SandboxContainer::getAllPids() const
{
    return subprocess_.getAllPids();
}

// ===========================================================================
// Profile generation
// ===========================================================================

std::string SandboxContainer::generateProfile(const LogosCore::ModuleDescriptor& desc,
                                               const std::string& hostBinary)
{
    std::ostringstream sb;
    sb << "(version 1)\n";
    sb << "(deny default)\n\n";

    // --- System essentials (always allowed) ---

    sb << "(allow file-read*\n"
       << "  (subpath \"/usr/lib\")\n"
       << "  (subpath \"/System/Library\")\n"
       << "  (subpath \"/Library/Frameworks\")\n"
       << "  (subpath \"/usr/share\")\n"
       << ")\n\n";

    sb << "(allow file-read*\n"
       << "  (subpath \"/private/var/db/dyld\")\n"
       << "  (literal \"/dev/null\")\n"
       << "  (literal \"/dev/urandom\")\n"
       << "  (literal \"/dev/random\")\n"
       << ")\n\n";

    // Host binary
    sb << "(allow file-read* (literal \"" << hostBinary << "\"))\n";
    sb << "(allow process-exec (literal \"" << hostBinary << "\"))\n";

    // Module path
    if (!desc.path.empty())
        sb << "(allow file-read* (subpath \"" << desc.path << "\"))\n";

    // Sibling module directories
    for (const auto& dir : desc.modulesDirs)
        sb << "(allow file-read* (subpath \"" << dir << "\"))\n";

    sb << "\n";

    // sysctl and Mach IPC
    sb << "(allow sysctl-read)\n";
    sb << "(allow mach-lookup)\n\n";

    // --- Token exchange socket ---

    std::string socketPath = tokenSocketPath(desc.name);
    std::string tmp = tmpDir();

    sb << "(allow network-unix)\n";
    sb << "(allow file-read* file-write* (literal \"" << socketPath << "\"))\n";
    sb << "(allow file-read* file-write* (subpath \"" << tmp << "\"))\n\n";

    // --- Persistence path ---

    if (!desc.instancePersistencePath.empty()) {
        sb << "(allow file-read* file-write* (subpath \""
           << desc.instancePersistencePath << "\"))\n\n";
    }

    // --- Configurable: network ---

    if (desc.runtimeConfig.contains("network")) {
        const auto& net = desc.runtimeConfig.at("network");

        if (net.contains("outbound")) {
            const auto& outbound = net.at("outbound");
            if (outbound.is_boolean() && outbound.get<bool>()) {
                sb << "(allow network-outbound)\n";
            } else if (outbound.is_array()) {
                for (const auto& entry : outbound)
                    sb << "(allow network-outbound (remote tcp \""
                       << entry.get<std::string>() << "\"))\n";
            }
        }

        if (net.contains("inbound")) {
            const auto& inbound = net.at("inbound");
            if (inbound.is_boolean() && inbound.get<bool>()) {
                sb << "(allow network-inbound)\n";
            } else if (inbound.is_array()) {
                for (const auto& entry : inbound)
                    sb << "(allow network-inbound (local tcp \"*:"
                       << entry.get<int>() << "\"))\n";
            }
        }
    }

    // --- Configurable: filesystem ---

    if (desc.runtimeConfig.contains("filesystem")) {
        const auto& fs = desc.runtimeConfig.at("filesystem");

        if (fs.contains("read")) {
            for (const auto& p : fs.at("read"))
                sb << "(allow file-read* (subpath \""
                   << p.get<std::string>() << "\"))\n";
        }

        if (fs.contains("read-write")) {
            for (const auto& p : fs.at("read-write"))
                sb << "(allow file-read* file-write* (subpath \""
                   << p.get<std::string>() << "\"))\n";
        }
    }

    return sb.str();
}

// ===========================================================================
// Profile file management
// ===========================================================================

std::string SandboxContainer::writeProfileFile(const std::string& moduleName,
                                                const std::string& profileContent)
{
    std::string path = tmpDir() + "/logos_sandbox_" + moduleName + ".sb";

    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) {
        spdlog::error("SandboxContainer: failed to write profile to {}", path);
        return "";
    }
    ofs << profileContent;
    ofs.close();
    return path;
}

void SandboxContainer::cleanupProfile(const std::string& name)
{
    std::string path;
    {
        std::lock_guard<std::mutex> lock(profilesMutex_);
        auto it = profilePaths_.find(name);
        if (it == profilePaths_.end()) return;
        path = it->second;
        profilePaths_.erase(it);
    }
    std::remove(path.c_str());
}
