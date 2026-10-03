#include "call_envelope.h"

#include <algorithm>

namespace logos::core_service {

namespace {

nlohmann::json methodNotFound(const std::string& module, const std::string& method,
                              const std::vector<std::string>& names)
{
    return {{"status", "error"},
            {"code", "METHOD_NOT_FOUND"},
            {"message", "Method '" + method + "' not found on module '" + module + "'."},
            {"available_methods", names}};
}

} // namespace

bool dispatchRejection(const nlohmann::json& value, CallFailure& out)
{
    if (!value.is_object() || value.size() != 3) return false;
    auto code = value.find("code"), message = value.find("message"), origin = value.find("origin");
    if (code == value.end() || message == value.end() || origin == value.end()) return false;
    if (!code->is_string() || !message->is_string() || !origin->is_string()) return false;
    const std::string c = code->get<std::string>();
    if (c != "dispatch_failed" && c != "invalid_args" && c != "unknown_method") return false;
    out = {c, message->get<std::string>(), origin->get<std::string>()};
    return true;
}

nlohmann::json callEnvelope(const std::string& module, const std::string& method,
                            const nlohmann::json& returned, CallFailure failure,
                            const MethodLister& listMethods)
{
    nlohmann::json result = nlohmann::json::object();
    if (failure.ok()) dispatchRejection(returned, failure);
    // The provider refused the NAME: the same envelope as the null-return rescue below.
    if (failure.code == "unknown_method")
        return methodNotFound(module, method,
                              listMethods ? listMethods() : std::vector<std::string>{});
    if (!failure.ok()) {
        result["status"] = "error";
        result["code"] = "METHOD_FAILED";
        result["message"] = "Call to " + module + "." + method + " failed (" + failure.code
            + ": " + failure.message + ").";
        result["error"] = {{"code", failure.code}, {"message", failure.message},
                           {"origin", failure.origin}};
        return result;
    }
    // A module built before providers refused unknown names answers one with a
    // bare null, as a void method does; only the module's own list can say.
    if (returned.is_null() && listMethods) {
        const std::vector<std::string> names = listMethods();
        if (!names.empty() && std::find(names.begin(), names.end(), method) == names.end())
            return methodNotFound(module, method, names);
    }
    result["status"] = "ok";
    result["module"] = module;
    result["method"] = method;
    result["result"] = returned;
    return result;
}

} // namespace logos::core_service
