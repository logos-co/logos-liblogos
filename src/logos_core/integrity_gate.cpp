#include "integrity_gate.h"

#include <lgx.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>

namespace fs = std::filesystem;

namespace LogosCore {

namespace {

// Resolves symlinks where the path exists, and drops a trailing separator, so
// two spellings of one directory compare equal.
fs::path normalised(const fs::path& p) {
    std::error_code ec;
    fs::path out = fs::weakly_canonical(p, ec);
    out = (ec ? p : out).lexically_normal();
    return out.filename().empty() ? out.parent_path() : out;
}

bool underVerifiedDir(const fs::path& installDir, const std::vector<std::string>& verifiedDirs) {
    const fs::path parent = normalised(installDir).parent_path();
    for (const std::string& dir : verifiedDirs) {
        if (!dir.empty() && normalised(dir) == parent)
            return true;
    }
    return false;
}

std::optional<std::string> readFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
        return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

IntegrityGateResult refuse(std::string reason) {
    IntegrityGateResult out;
    out.decision = IntegrityGateDecision::Refuse;
    out.reason = std::move(reason);
    return out;
}

} // namespace

IntegrityGateResult evaluateIntegrityGate(const std::string& installDir,
                                          const std::vector<std::string>& verifiedDirs)
{
    if (installDir.empty() || !underVerifiedDir(installDir, verifiedDirs))
        return {};
    return checkInstalledTree(installDir);
}

IntegrityGateResult checkInstalledTree(const std::string& installDir)
{
    if (installDir.empty())
        return refuse("no install directory to check");

    const fs::path dir(installDir);
    const std::optional<std::string> manifest = readFile(dir / "manifest.json");
    if (!manifest)
        return refuse("installed module has no readable manifest.json");

    std::optional<std::string> variant = readFile(dir / "variant");
    if (!variant)
        return refuse("installed module has no readable variant file");
    while (!variant->empty() && (variant->back() == '\n' || variant->back() == '\r' ||
                                 variant->back() == ' '))
        variant->pop_back();

    const lgx_integrity_t integrity = lgx_verify_installed_tree(
        dir.string().c_str(), manifest->data(), manifest->size(), variant->c_str());
    if (integrity == LGX_INTEGRITY_OK)
        return IntegrityGateResult{IntegrityGateDecision::Allow, {}};

    const char* detail = lgx_get_last_error();
    const std::string why = detail && *detail ? std::string(": ") + detail : std::string();
    switch (integrity) {
    case LGX_INTEGRITY_MISMATCH:
        return refuse("installed module has been modified or damaged since install" + why);
    case LGX_INTEGRITY_NO_HASH:
        return refuse("installed module's manifest carries no content hashes" + why);
    case LGX_INTEGRITY_UNREADABLE:
        return refuse("installed module could not be read" + why);
    default:
        return refuse("installed module could not be checked" + why);
    }
}

} // namespace LogosCore
