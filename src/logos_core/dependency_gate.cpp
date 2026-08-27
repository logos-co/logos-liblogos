#include "dependency_gate.h"

#include <lgx.h>
#include <nlohmann/json.hpp>

namespace LogosCore {
namespace {

std::string jsonString(const nlohmann::json& obj, const char* key) {
    auto it = obj.find(key);
    return (it != obj.end() && it->is_string()) ? it->get<std::string>()
                                                : std::string{};
}

}  // namespace

DependencyGateResult evaluateDependencyGate(
    const std::vector<ModuleDependency>& dependencies,
    const std::function<std::string(const std::string&)>& installedVersionOf)
{
    DependencyGateResult out;

    for (const ModuleDependency& dep : dependencies) {
        if (dep.versionRange.empty())
            continue;

        const std::string installed =
            installedVersionOf ? installedVersionOf(dep.name) : std::string();

        const char* problem = nullptr;
        if (!lgx_semver_valid_range(dep.versionRange.c_str()))
            problem = "declares an unparseable version range";
        else if (installed.empty())
            problem = "has no readable installed version";
        else if (!lgx_semver_satisfies(installed.c_str(), dep.versionRange.c_str()))
            problem = "is out of range";

        if (problem) {
            out.decision = DependencyGateDecision::Refuse;
            out.dependency = dep.name;
            out.range = dep.versionRange;
            out.installedVersion = installed;
            out.reason = "dependency " + dep.name + " " + problem + ": requires " +
                         dep.versionRange + ", installed " +
                         (installed.empty() ? std::string("(unknown)") : installed);
            return out;
        }
        out.decision = DependencyGateDecision::Allow;
    }

    return out;
}

EmbeddedDeclaration parseEmbeddedDeclaration(const std::string& metadataJson)
{
    EmbeddedDeclaration out;
    if (metadataJson.empty()) return out;
    nlohmann::json meta = nlohmann::json::parse(metadataJson, nullptr,
                                                /*allow_exceptions=*/false);
    if (!meta.is_object()) return out;

    out.version = jsonString(meta, "version");

    auto deps = meta.find("dependencies");
    if (deps == meta.end() || !deps->is_array()) return out;
    for (const nlohmann::json& dep : *deps) {
        ModuleDependency entry;
        if (dep.is_object()) {
            entry.name         = jsonString(dep, "name");
            entry.versionRange = jsonString(dep, "version");
            entry.signer       = jsonString(dep, "signer");
        } else if (dep.is_string()) {
            entry.name = dep.get<std::string>();
        }
        if (!entry.name.empty()) out.dependencies.push_back(std::move(entry));
    }
    return out;
}

} // namespace LogosCore
