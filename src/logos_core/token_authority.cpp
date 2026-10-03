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
    bool version2 = false;
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

// The engine, when it has version 2's entries.
const logos_capability_engine_v1* current2()
{
    std::lock_guard<std::mutex> lock(state().mutex);
    return state().version2 ? state().engine : nullptr;
}

bool hasVersion2Entries(const logos_capability_engine_v1* engine)
{
    return engine->version >= 2 && engine->size >= sizeof(logos_capability_engine_v1)
        && engine->set_access_rules && engine->grant_for && engine->admit_pending
        && engine->open_target;
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
    if (!engine || engine->size < LOGOS_CAPABILITY_ENGINE_V1_SIZE || engine->version < 1) {
        spdlog::critical("capability_module's engine interface is missing or malformed");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().engine = engine;
        state().version2 = hasVersion2Entries(engine);
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
    state().version2 = false;
    state().generations.clear();
}

bool attached()
{
    return current() != nullptr;
}

bool isVersion2()
{
    return current2() != nullptr;
}

std::string admit(const std::string& name, const std::string& kind, bool pending)
{
    const logos_capability_engine_v1* engine = current();
    if (!engine) return {};
    const auto admitFn = pending && hasVersion2Entries(engine) ? engine->admit_pending : engine->admit;
    unsigned long long generation = 0;
    const std::string credential =
        take(engine, admitFn(name.c_str(), kind.c_str(), &generation));
    if (credential.empty()) return {};
    std::lock_guard<std::mutex> lock(state().mutex);
    state().generations[name] = generation;
    return credential;
}

bool openTarget(const std::string& name)
{
    const logos_capability_engine_v1* engine = current();
    if (!engine) return false;
    if (!hasVersion2Entries(engine)) return true;
    return engine->open_target(name.c_str()) == 0;
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

bool setAccessRules(const std::string& json)
{
    const logos_capability_engine_v1* engine = current2();
    return engine && engine->set_access_rules(json.c_str()) == 0;
}

std::optional<std::string> grantFor(const std::string& caller, const std::string& target)
{
    const logos_capability_engine_v1* engine = current();
    if (!engine) return std::nullopt;
    if (!hasVersion2Entries(engine)) return std::string("\"*\"");
    const std::string grant = take(engine, engine->grant_for(caller.c_str(), target.c_str()));
    if (grant.empty()) return std::nullopt;
    return grant;
}

char* resolveCallerCallback(const char* token, const char* transport, void*)
{
    const auto document = resolveCaller(token, transport);
    return document ? lp_string_copy(document->c_str()) : nullptr;
}

} // namespace logos::authority
