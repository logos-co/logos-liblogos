#include "access_policy.h"

#include <nlohmann/json.hpp>

namespace LogosCore {

std::optional<AccessPolicy> parseAccessPolicy(const std::string& json)
{
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (const std::exception&) {
        return std::nullopt;  // invalid JSON is the only hard failure
    }

    if (!doc.is_object())
        return std::nullopt;

    AccessPolicy policy;
    policy.version = doc.value("version", 0);
    policy.mode    = doc.value("mode", std::string{});

    auto restrictionsIt = doc.find("restrictions");
    if (restrictionsIt != doc.end() && restrictionsIt->is_object()) {
        for (auto it = restrictionsIt->begin(); it != restrictionsIt->end(); ++it) {
            const std::string& target = it.key();
            if (target.empty())
                continue;

            AccessRestriction restriction;
            restriction.target = target;

            const auto& entry = it.value();
            if (entry.is_object()) {
                auto callersIt = entry.find("allowedCallers");
                if (callersIt != entry.end() && callersIt->is_array()) {
                    for (const auto& caller : *callersIt) {
                        if (caller.is_string())
                            restriction.allowedCallers.push_back(
                                caller.get<std::string>());
                    }
                }
            }

            policy.restrictions.push_back(std::move(restriction));
        }
    }

    return policy;
}

} // namespace LogosCore
