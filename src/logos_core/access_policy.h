#ifndef ACCESS_POLICY_H
#define ACCESS_POLICY_H

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

// Inter-module access policy model + parser (Qt-free). Parses the JSON set
// via logos_core_set_access_policy(), e.g.
//   {"version":1,"mode":"enforce","restrictions":{
//     "package_manager":{"allowedCallers":["package_manager_ui"]}}}
// Version 2 also grants methods:
//   {"version":2,"mode":"explicit","restrictions":{
//     "keystore_module":{"allowedCallers":{"evm_keystore_ui":"*",
//                                          "@op:*":["list_accounts"]}}}}

namespace LogosCore {

// One target module and the callers permitted to reach it: a list of callers, or
// (version 2) each caller's grant, "*" or its methods.
struct AccessRestriction {
    std::string target;
    std::vector<std::string> allowedCallers;
    std::optional<nlohmann::json> grants;
};

struct AccessPolicy {
    int version = 0;
    std::string mode;
    std::vector<AccessRestriction> restrictions;

    // "enforce": the rules written plus those derived from declared dependencies.
    // "explicit": only the rules written. "off", or no mode: every target open.
    bool enforce() const { return mode == "enforce"; }
    bool explicitOnly() const { return mode == "explicit"; }
    bool active() const { return enforce() || explicitOnly(); }
};

// nullopt, with why in `error`, for a policy the runtime refuses: invalid JSON, a
// field of the wrong type, or an unknown version or mode. Version 1 ignores
// unknown keys and non-string callers, and a target without allowedCallers names
// none. Version 2 takes exactly its grammar, and never names the runtime's own
// core, core_service or capability_module.
std::optional<AccessPolicy> parseAccessPolicy(const std::string& json,
                                              std::string* error = nullptr);

} // namespace LogosCore

#endif // ACCESS_POLICY_H
