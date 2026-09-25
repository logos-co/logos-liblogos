#ifndef TOKEN_AUTHORITY_H
#define TOKEN_AUTHORITY_H

// The engine's side of logos_capability_engine_v1. capability_module runs
// in-process and exports it; it mints every credential but its own, names every
// caller and enforces the access policy. Without it nothing is admitted, so
// nothing loads.

#include <optional>
#include <string>

struct logos_capability_engine_v1;
struct lp_provider;

namespace logos::authority {

// Attaches `engine` and lets capability's own provider name callers through it.
bool attach(const logos_capability_engine_v1* engine, lp_provider* capabilityProvider);
void detach();
bool attached();

// The credential of a new admission of `name` ("module", "shell", "presentation"),
// or "" when refused; retire() ends the latest one.
std::string admit(const std::string& name, const std::string& kind);
void retire(const std::string& name);

// The caller document for a credential, as a provider's caller resolver answers.
std::optional<std::string> resolveCaller(const char* token, const char* transport);

// A token for operator `op` to call `target`, or "".
std::string grantOperatorPair(const std::string& op, const std::string& target);

// Replaces the access policy, {"<target>":["<caller>",...]}; false when refused.
bool setRestrictions(const std::string& json);

// For a provider's lp_provider_set_caller_resolver: callers capability knows.
char* resolveCallerCallback(const char* token, const char* transport, void* userData);

} // namespace logos::authority

#endif // TOKEN_AUTHORITY_H
