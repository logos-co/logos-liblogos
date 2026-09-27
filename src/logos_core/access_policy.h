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

    // Only "enforce" turns restrictions into denials; "off", or no mode, leaves
    // every target open. The parser refuses any other mode.
    bool enforce() const { return mode == "enforce"; }
};

// nullopt, with why in `error`, for a policy the runtime refuses: invalid JSON, a
// field of the wrong type, or an unknown version or mode. Unknown keys and
// non-string callers are ignored; a target without allowedCallers names none.
std::optional<AccessPolicy> parseAccessPolicy(const std::string& json,
                                              std::string* error = nullptr);

} // namespace LogosCore

#endif // ACCESS_POLICY_H
