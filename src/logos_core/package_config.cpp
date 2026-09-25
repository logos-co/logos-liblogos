#include "package_config.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <mutex>
#include <set>

using json = nlohmann::json;

namespace logos::package_config {

namespace {

std::mutex& mutex()
{
    static std::mutex value;
    return value;
}

std::vector<Call>& stored()
{
    static std::vector<Call> value;
    return value;
}

} // namespace

bool parse(const std::string& text, std::vector<Call>& out, std::string& error)
{
    const json doc = json::parse(text, nullptr, false);
    if (!doc.is_object()) {
        error = "the package config is not a JSON object";
        return false;
    }
    static const std::set<std::string> known{
        "embedded_modules_dirs", "user_modules_dir", "embedded_ui_plugins_dirs",
        "user_ui_plugins_dir", "keyring_dir", "signature_policy"};
    for (const auto& [key, value] : doc.items()) {
        if (!known.count(key)) {
            error = "unknown package config key '" + key + "'";
            return false;
        }
    }
    std::vector<Call> calls;
    auto dirs = [&](const char* key, const char* first, const char* more) {
        auto it = doc.find(key);
        if (it == doc.end()) return true;
        if (!it->is_array()) {
            error = std::string(key) + " must be an array of paths";
            return false;
        }
        bool firstOfKey = true;
        for (const auto& dir : *it) {
            if (!dir.is_string() || dir.get<std::string>().empty()) {
                error = std::string(key) + " must hold non-empty paths";
                return false;
            }
            calls.push_back({firstOfKey ? first : more, dir.get<std::string>()});
            firstOfKey = false;
        }
        return true;
    };
    auto one = [&](const char* key, const char* method, bool failClosed) {
        auto it = doc.find(key);
        if (it == doc.end()) return true;
        if (!it->is_string() || it->get<std::string>().empty()) {
            error = std::string(key) + " must be a non-empty string";
            return false;
        }
        calls.push_back({method, it->get<std::string>(), failClosed});
        return true;
    };
    if (!dirs("embedded_modules_dirs", "setEmbeddedModulesDirectory", "addEmbeddedModulesDirectory")
        || !one("user_modules_dir", "setUserModulesDirectory", false)
        || !dirs("embedded_ui_plugins_dirs", "setEmbeddedUiPluginsDirectory",
                 "addEmbeddedUiPluginsDirectory")
        || !one("user_ui_plugins_dir", "setUserUiPluginsDirectory", false)
        || !one("keyring_dir", "setKeyringDirectory", false)
        || !one("signature_policy", "setSignaturePolicy", true))
        return false;
    if (auto it = doc.find("signature_policy"); it != doc.end()) {
        const std::string policy = it->get<std::string>();
        if (policy != "none" && policy != "warn" && policy != "require") {
            error = "signature_policy is none, warn or require";
            return false;
        }
    }
    out = std::move(calls);
    return true;
}

void set(std::vector<Call> calls)
{
    std::lock_guard<std::mutex> lock(mutex());
    stored() = std::move(calls);
}

std::vector<Call> current()
{
    std::lock_guard<std::mutex> lock(mutex());
    return stored();
}

void reset()
{
    set({});
}

bool apply(const std::vector<Call>& calls,
           const std::function<bool(const std::string&, const std::string&)>& invoke,
           std::string& error)
{
    for (const Call& call : calls) {
        if (invoke(call.method, call.arg)) continue;
        if (call.failClosed) {
            error = "package_manager did not take " + call.method + "('" + call.arg + "')";
            return false;
        }
        spdlog::warn("package_manager did not take {}('{}')", call.method, call.arg);
    }
    return true;
}

} // namespace logos::package_config
