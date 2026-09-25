#ifndef CORE_SERVICE_CALL_ENVELOPE_H
#define CORE_SERVICE_CALL_ENVELOPE_H

// callModuleMethod's answer, shaped as logosctl reads it: the module's value, or
// what went wrong, told apart from a method that returned nothing.

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace logos::core_service {

struct CallFailure {
    std::string code;
    std::string message;
    std::string origin;

    bool ok() const { return code.empty(); }
};

// A provider's refusal of a dispatch, sent as an ordinary value.
bool dispatchRejection(const nlohmann::json& value, CallFailure& out);

using MethodLister = std::function<std::vector<std::string>()>;

nlohmann::json callEnvelope(const std::string& module, const std::string& method,
                            const nlohmann::json& returned, CallFailure failure,
                            const MethodLister& listMethods);

} // namespace logos::core_service

#endif
