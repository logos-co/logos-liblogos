#ifndef TOKEN_AUTHORITY_H
#define TOKEN_AUTHORITY_H

// The engine's side of logos_capability_engine_v1. Once capability_module runs
// in-process and exports it, capability mints every credential and names every
// caller; until then, or without it, core mints credentials as it always did.

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

// For a provider's lp_provider_set_caller_resolver: callers capability knows.
char* resolveCallerCallback(const char* token, const char* transport, void* userData);

} // namespace logos::authority

#endif // TOKEN_AUTHORITY_H
