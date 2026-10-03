#include "access_policy.h"
#include "module_registry.h"

#include <logos_method_scope.h>

#include <cctype>
#include <set>

namespace LogosCore {
namespace {

using json = nlohmann::json;

bool isRuntimeName(const std::string& name)
{
    return name == "core" || name == "core_service" || name == "capability_module";
}

// An operator token's name, as logosctl issues them.
bool isOperatorName(const std::string& name)
{
    if (name.empty() || name.size() > 64 || name == "." || name == "..") return false;
    for (const unsigned char c : name)
        if (!std::isalnum(c) && c != '.' && c != '-' && c != '_') return false;
    return true;
}

// A peered runtime's consumer, as core_service names it: "@peer:<runtime id>:<consumer>",
// each as libpeering admits it (a lowercase 8-4-4-4-12 hex id; a letter, then [A-Za-z0-9_]).
bool isPeerOperatorName(const std::string& name)
{
    const std::string prefix = "@peer:";
    constexpr std::size_t kId = 36;
    constexpr std::size_t kMaxConsumer = 128;
    if (name.rfind(prefix, 0) != 0 || name.size() < prefix.size() + kId + 2) return false;
    for (std::size_t i = 0; i < kId; ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        const char c = name[prefix.size() + i];
        if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    if (name[prefix.size() + kId] != ':') return false;
    const std::string consumer = name.substr(prefix.size() + kId + 1);
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (consumer.size() > kMaxConsumer || !letter(consumer[0])) return false;
    for (const char c : consumer)
        if (!letter(c) && !(c >= '0' && c <= '9') && c != '_') return false;
    return true;
}

// "*", "@op:*", "@op:<operator>", or a module, UI or shell name.
bool isCallerKey(const std::string& caller)
{
    if (caller == "*" || caller == "@op:*") return true;
    if (caller.rfind("@op:", 0) == 0)
        return isOperatorName(caller.substr(4)) || isPeerOperatorName(caller.substr(4));
    return logos::isValidModuleName(caller);
}

// "*", or a list of methods a target would take as a scope; [] grants none.
bool isGrant(const json& grant)
{
    if (grant.is_string()) return grant.get<std::string>() == "*";
    if (!grant.is_array()) return false;
    return grant.empty() || logos::parseMethodScope(json{{"methods", grant}}.dump());
}

std::optional<AccessRestriction> parseVersion2Rule(const std::string& target, const json& entry,
                                                   std::string& why)
{
    if (!logos::isValidModuleName(target) || isRuntimeName(target)) {
        why = "a version 2 rule cannot restrict '" + target + "'";
        return std::nullopt;
    }
    if (!entry.is_object()) {
        why = "the rule for '" + target + "' must be an object with allowedCallers";
        return std::nullopt;
    }
    for (const auto& [key, value] : entry.items()) {
        if (key != "allowedCallers") {
            why = "the rule for '" + target + "' has an unknown key '" + key + "'";
            return std::nullopt;
        }
    }
    AccessRestriction rule;
    rule.target = target;
    const auto callers = entry.find("allowedCallers");
    if (callers == entry.end()) return rule;
    const auto badCaller = [&](const std::string& caller) {
        if (!isCallerKey(caller))
            why = "'" + caller + "' in the rule for '" + target + "' is not a caller";
        else if (isRuntimeName(caller))
            why = "a version 2 rule cannot name the runtime's own '" + caller + "'";
        else
            return false;
        return true;
    };
    if (callers->is_array()) {
        std::set<std::string> seen;
        for (const auto& caller : *callers) {
            const std::string name = caller.is_string() ? caller.get<std::string>() : std::string{};
            if (badCaller(name)) return std::nullopt;
            if (!seen.insert(name).second) {
                why = "'" + name + "' is listed twice for '" + target + "'";
                return std::nullopt;
            }
            rule.allowedCallers.push_back(name);
        }
        return rule;
    }
    if (!callers->is_object()) {
        why = "allowedCallers of '" + target + "' must be a list or an object";
        return std::nullopt;
    }
    for (const auto& [caller, grant] : callers->items()) {
        if (badCaller(caller)) return std::nullopt;
        if (!isGrant(grant)) {
            why = "the grant of '" + caller + "' at '" + target
                + "' must be \"*\" or a list of distinct method names";
            return std::nullopt;
        }
    }
    rule.grants = *callers;
    return rule;
}

} // namespace

std::optional<AccessPolicy> parseAccessPolicy(const std::string& json, std::string* error)
{
    const auto refuse = [error](std::string why) -> std::optional<AccessPolicy> {
        if (error) *error = std::move(why);
        return std::nullopt;
    };
    const nlohmann::json doc = nlohmann::json::parse(json, nullptr, false);
    if (doc.is_discarded()) return refuse("the access policy is not valid JSON");
    if (!doc.is_object()) return refuse("the access policy is not a JSON object");

    AccessPolicy policy;
    if (const auto it = doc.find("version"); it != doc.end()) {
        if (!it->is_number_integer()) return refuse("\"version\" must be an integer");
        const long long version = it->get<long long>();
        if (version < 0 || version > 2)
            return refuse("access-policy version " + it->dump() + " is not known");
        policy.version = static_cast<int>(version);
    }
    const bool version2 = policy.version == 2;
    if (version2)
        for (const auto& [key, value] : doc.items())
            if (key != "version" && key != "mode" && key != "restrictions")
                return refuse("a version 2 access policy has no '" + key + "'");
    if (const auto it = doc.find("mode"); it != doc.end()) {
        if (!it->is_string()) return refuse("\"mode\" must be a string");
        policy.mode = it->get<std::string>();
        if (policy.mode != "enforce" && policy.mode != "explicit" && policy.mode != "off")
            return refuse("access-policy mode \"" + policy.mode + "\" is not known");
    }

    const auto restrictionsIt = doc.find("restrictions");
    if (restrictionsIt == doc.end()) return policy;
    if (!restrictionsIt->is_object()) return refuse("\"restrictions\" must be an object");
    for (const auto& [target, entry] : restrictionsIt->items()) {
        if (version2) {
            std::string why;
            auto rule = parseVersion2Rule(target, entry, why);
            if (!rule) return refuse(why);
            policy.restrictions.push_back(std::move(*rule));
            continue;
        }
        if (target.empty()) continue;
        if (!entry.is_object())
            return refuse("the rule for '" + target + "' must be an object with allowedCallers");
        AccessRestriction restriction;
        restriction.target = target;
        if (const auto callersIt = entry.find("allowedCallers"); callersIt != entry.end()) {
            if (callersIt->is_object())
                return refuse("allowedCallers of '" + target + "' grants methods, which "
                              "needs version 2");
            if (!callersIt->is_array())
                return refuse("allowedCallers of '" + target + "' must be a list");
            for (const auto& caller : *callersIt)
                if (caller.is_string()) restriction.allowedCallers.push_back(caller.get<std::string>());
        }
        policy.restrictions.push_back(std::move(restriction));
    }
    return policy;
}

} // namespace LogosCore
