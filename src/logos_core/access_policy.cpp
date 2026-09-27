#include "access_policy.h"

#include <nlohmann/json.hpp>

namespace LogosCore {

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
        if (it->get<long long>() != 0 && it->get<long long>() != 1)
            return refuse("access-policy version " + it->dump() + " is not known");
        policy.version = it->get<int>();
    }
    if (const auto it = doc.find("mode"); it != doc.end()) {
        if (!it->is_string()) return refuse("\"mode\" must be a string");
        policy.mode = it->get<std::string>();
        if (policy.mode != "enforce" && policy.mode != "off")
            return refuse("access-policy mode \"" + policy.mode + "\" is not known");
    }

    const auto restrictionsIt = doc.find("restrictions");
    if (restrictionsIt == doc.end()) return policy;
    if (!restrictionsIt->is_object()) return refuse("\"restrictions\" must be an object");
    for (const auto& [target, entry] : restrictionsIt->items()) {
        if (target.empty()) continue;
        if (!entry.is_object())
            return refuse("the rule for '" + target + "' must be an object with allowedCallers");
        AccessRestriction restriction;
        restriction.target = target;
        if (const auto callersIt = entry.find("allowedCallers"); callersIt != entry.end()) {
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
