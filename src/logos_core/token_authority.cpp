#include "token_authority.h"

#include <logos_capability_engine.h>
#include <logos_protocol.h>
#include <spdlog/spdlog.h>

#include <mutex>
#include <unordered_map>

namespace logos::authority {
namespace {

struct State {
    std::mutex mutex;
    const logos_capability_engine_v1* engine = nullptr;
    std::unordered_map<std::string, unsigned long long> generations;
};

State& state()
{
    static State value;
    return value;
}

const logos_capability_engine_v1* current()
{
    std::lock_guard<std::mutex> lock(state().mutex);
    return state().engine;
}

std::string take(const logos_capability_engine_v1* engine, char* value)
{
    std::string text = value ? value : "";
    if (value) engine->string_free(value);
    return text;
}

} // namespace

bool attach(const logos_capability_engine_v1* engine, lp_provider* capabilityProvider)
{
    // Every version-1 authority has the entries up to string_free; later ones are
    // looked for where they are used.
    if (!engine || !LOGOS_CAPABILITY_ENGINE_HAS(engine, string_free)
        || engine->version < LOGOS_CAPABILITY_ENGINE_VERSION) {
        spdlog::critical("capability_module's engine interface is missing or older than "
                         "version {}", LOGOS_CAPABILITY_ENGINE_VERSION);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().engine = engine;
    }
    if (capabilityProvider
        && lp_provider_set_caller_resolver(capabilityProvider, &resolveCallerCallback, nullptr)
               != LP_OK)
        spdlog::warn("capability_module's provider takes no caller resolver");
    spdlog::info("capability_module is the token authority");
    return true;
}

void detach()
{
    std::lock_guard<std::mutex> lock(state().mutex);
    state().engine = nullptr;
    state().generations.clear();
}

bool attached()
{
    return current() != nullptr;
}

std::string admit(const std::string& name, const std::string& kind)
{
    const logos_capability_engine_v1* engine = current();
    if (!engine) return {};
    unsigned long long generation = 0;
    const std::string credential =
        take(engine, engine->admit(name.c_str(), kind.c_str(), &generation));
    if (credential.empty()) return {};
    std::lock_guard<std::mutex> lock(state().mutex);
    state().generations[name] = generation;
    return credential;
}

void retire(const std::string& name)
{
    const logos_capability_engine_v1* engine = nullptr;
    unsigned long long generation = 0;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        engine = state().engine;
        auto it = state().generations.find(name);
        if (!engine || it == state().generations.end()) return;
        generation = it->second;
        state().generations.erase(it);
    }
    engine->retire(name.c_str(), generation);
}

std::optional<std::string> resolveCaller(const char* token, const char* transport)
{
    const logos_capability_engine_v1* engine = current();
    if (!engine || !token) return std::nullopt;
    const std::string document = take(engine, engine->resolve_caller(token, transport));
    if (document.empty()) return std::nullopt;
    return document;
}

std::string grantOperatorPair(const std::string& op, const std::string& target)
{
    const logos_capability_engine_v1* engine = current();
    if (!engine) return {};
    return take(engine, engine->grant_operator_pair(op.c_str(), target.c_str()));
}

bool setRestrictions(const std::string& json)
{
    const logos_capability_engine_v1* engine = current();
    return engine && engine->set_restrictions(json.c_str()) == 0;
}

std::optional<std::string> evaluateRemoteAccess(const std::string& peer, const std::string& consumer,
                                                const std::string& target)
{
    const logos_capability_engine_v1* engine = current();
    if (!engine || !LOGOS_CAPABILITY_ENGINE_HAS(engine, evaluate_remote_access)) return std::nullopt;
    const std::string decision =
        take(engine, engine->evaluate_remote_access(peer.c_str(), consumer.c_str(), target.c_str()));
    if (decision.empty()) return std::nullopt;
    return decision;
}

bool setRemotePolicy(const std::string& json)
{
    const logos_capability_engine_v1* engine = current();
    return engine && LOGOS_CAPABILITY_ENGINE_HAS(engine, set_remote_policy)
        && engine->set_remote_policy(json.c_str()) == 0;
}

bool setCallerScopes(const std::string& json)
{
    const logos_capability_engine_v1* engine = current();
    return engine && LOGOS_CAPABILITY_ENGINE_HAS(engine, set_caller_scopes)
        && engine->set_caller_scopes(json.c_str()) == 0;
}

char* resolveCallerCallback(const char* token, const char* transport, void*)
{
    const auto document = resolveCaller(token, transport);
    return document ? lp_string_copy(document->c_str()) : nullptr;
}

} // namespace logos::authority
