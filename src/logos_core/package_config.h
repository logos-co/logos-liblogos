#ifndef PACKAGE_CONFIG_H
#define PACKAGE_CONFIG_H

// The package modules' settings, from the embedder: package_manager's setters
// answer only the runtime and core_service (logos-package-manager-module#72), so
// the runtime applies them before package_manager counts as loaded.

#include <functional>
#include <string>
#include <vector>

namespace logos::package_config {

struct Call {
    std::string method;
    std::string arg;
    // A signature policy that does not land fails the load: fail closed.
    bool failClosed = false;
};

// The setter calls a JSON document asks for, in the order they apply; false
// with `error` for a malformed one.
bool parse(const std::string& json, std::vector<Call>& calls, std::string& error);

void set(std::vector<Call> calls);
std::vector<Call> current();
void reset();

// Runs `calls` through `invoke`; false with `error` when a fail-closed one did
// not land. Other failures are logged and skipped.
bool apply(const std::vector<Call>& calls,
           const std::function<bool(const std::string& method, const std::string& arg)>& invoke,
           std::string& error);

} // namespace logos::package_config

#endif
