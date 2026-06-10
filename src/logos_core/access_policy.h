#ifndef ACCESS_POLICY_H
#define ACCESS_POLICY_H

#include <optional>
#include <string>
#include <vector>

// Inter-module access policy model + parser (Qt-free). Parses the JSON set
// via logos_core_set_access_policy(), e.g.
//   {"version":1,"mode":"enforce","restrictions":{
//     "package_manager":{"allowedCallers":["package_manager_ui"]}}}

namespace LogosCore {

// One target module and the set of caller modules permitted to reach it.
struct AccessRestriction {
    std::string target;
    std::vector<std::string> allowedCallers;
};

struct AccessPolicy {
    int version = 0;
    std::string mode;
    std::vector<AccessRestriction> restrictions;

    // Only "enforce" turns restrictions into denials; any other value
    // leaves the policy informational and core registers nothing.
    bool enforce() const { return mode == "enforce"; }
};

// Returns nullopt only on invalid JSON. Otherwise tolerant: unknown keys
// ignored, missing "restrictions"/"allowedCallers" yield empty lists.
std::optional<AccessPolicy> parseAccessPolicy(const std::string& json);

} // namespace LogosCore

#endif // ACCESS_POLICY_H
