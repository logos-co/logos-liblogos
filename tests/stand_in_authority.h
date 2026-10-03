// A stand-in for capability_module's engine interface.
//
// The real capability_module can run in-process only once per process, and this
// test binary runs every case in one. So every case is admitted by this
// instead: it mints a credential per admission, names the callers it admitted,
// grants operator pairs, and records every access-policy document the runtime
// sends and every admission step. InprocBundledTest detaches it and runs the
// real one.
#pragma once

#include "logos_capability_engine.h"
#include "token_authority.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace stand_in {

struct Admission {
    std::string credential;
    unsigned long long generation = 0;
    bool open = true;
};

struct State {
    std::mutex mutex;
    std::map<std::string, Admission> admitted;                                  // name -> admission
    std::map<std::string, std::string> names;                                   // credential -> name
    std::vector<std::string> restrictions;                                       // every document, in order
    std::vector<std::string> events;                                             // "admit_pending:x", "open:x", "rules", ...
    std::map<std::pair<std::string, std::string>, std::string> grants;          // (caller, target) -> grant JSON
    std::function<void(const nlohmann::json&)> onRestrictions;                   // runs before one is recorded
    std::function<void(const std::string&)> onEvent;                             // runs after one is recorded
    bool refuseRules = false;
    unsigned long long next = 1;
};

inline State& state()
{
    static State value;
    return value;
}

inline char* copy(const std::string& value)
{
    char* out = static_cast<char*>(std::malloc(value.size() + 1));
    std::memcpy(out, value.c_str(), value.size() + 1);
    return out;
}

inline void record(const std::string& event)
{
    std::function<void(const std::string&)> hook;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().events.push_back(event);
        hook = state().onEvent;
    }
    if (hook) hook(event);
}

inline char* admitAs(const char* name, const char* kind, unsigned long long* generation, bool open)
{
    const std::string k = kind ? kind : "";
    if (!name || !*name || (k != "module" && k != "shell" && k != "presentation")) return nullptr;
    std::string credential;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        State& s = state();
        if (auto it = s.admitted.find(name); it != s.admitted.end()) s.names.erase(it->second.credential);
        const unsigned long long g = s.next++;
        credential = "stand-in-" + std::string(name) + "-" + std::to_string(g);
        s.admitted[name] = {credential, g, open};
        s.names[credential] = name;
        if (generation) *generation = g;
    }
    record((open ? "admit:" : "admit_pending:") + std::string(name));
    return copy(credential);
}

inline char* admit(const char* name, const char* kind, unsigned long long* generation)
{
    return admitAs(name, kind, generation, true);
}

inline char* admitPending(const char* name, const char* kind, unsigned long long* generation)
{
    return admitAs(name, kind, generation, false);
}

inline int openTarget(const char* name)
{
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        auto it = name ? state().admitted.find(name) : state().admitted.end();
        if (it == state().admitted.end()) return -1;
        it->second.open = true;
    }
    record("open:" + std::string(name));
    return 0;
}

inline int retire(const char* name, unsigned long long generation)
{
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        State& s = state();
        auto it = name ? s.admitted.find(name) : s.admitted.end();
        if (it == s.admitted.end() || it->second.generation != generation) return -1;
        s.names.erase(it->second.credential);
        s.admitted.erase(it);
    }
    record("retire:" + std::string(name));
    return 0;
}

inline char* resolveCaller(const char* token, const char*)
{
    std::lock_guard<std::mutex> lock(state().mutex);
    auto it = token ? state().names.find(token) : state().names.end();
    if (it == state().names.end()) return nullptr;
    return copy(nlohmann::json{{"kind", "module"}, {"name", it->second}}.dump());
}

inline char* credentialFor(const char* name)
{
    std::lock_guard<std::mutex> lock(state().mutex);
    auto it = name ? state().admitted.find(name) : state().admitted.end();
    return it == state().admitted.end() ? nullptr : copy(it->second.credential);
}

inline char* grantOperatorPair(const char* op, const char* target)
{
    std::lock_guard<std::mutex> lock(state().mutex);
    auto it = target ? state().admitted.find(target) : state().admitted.end();
    if (!op || it == state().admitted.end() || !it->second.open) return nullptr;
    return copy("stand-in-op-" + std::string(op) + "-" + target);
}

inline int takeRules(const char* document, const char* entry)
{
    const nlohmann::json parsed = nlohmann::json::parse(document ? document : "", nullptr, false);
    if (!parsed.is_object()) return -1;
    std::function<void(const nlohmann::json&)> hook;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        if (state().refuseRules) return -1;
        hook = state().onRestrictions;
    }
    if (hook) hook(parsed);
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().restrictions.push_back(parsed.dump());
    }
    record(entry);
    return 0;
}

inline int setRestrictions(const char* document)
{
    return takeRules(document, "set_restrictions");
}

inline int setAccessRules(const char* document)
{
    return takeRules(document, "set_access_rules");
}

inline char* grantFor(const char* caller, const char* target)
{
    std::lock_guard<std::mutex> lock(state().mutex);
    const auto it = state().grants.find({caller ? caller : "", target ? target : ""});
    return copy(it == state().grants.end() ? std::string("\"*\"") : it->second);
}

inline void stringFree(char* value) { std::free(value); }

inline const logos_capability_engine_v1& engine()
{
    static const logos_capability_engine_v1 table = {
        sizeof(logos_capability_engine_v1), LOGOS_CAPABILITY_ENGINE_VERSION,
        &admit, &retire, &resolveCaller, &credentialFor, &grantOperatorPair,
        &setRestrictions, &stringFree,
        &setAccessRules, &grantFor, &admitPending, &openTarget,
    };
    return table;
}

// The table an older capability_module hands over: version 1, and only its entries.
inline const logos_capability_engine_v1& engineVersion1()
{
    static const logos_capability_engine_v1 table = {
        LOGOS_CAPABILITY_ENGINE_V1_SIZE, 1,
        &admit, &retire, &resolveCaller, &credentialFor, &grantOperatorPair,
        &setRestrictions, &stringFree,
        nullptr, nullptr, nullptr, nullptr,
    };
    return table;
}

// Attaches the stand-in unless an authority is attached already.
inline void attach()
{
    if (!logos::authority::attached()) logos::authority::attach(&engine(), nullptr);
}

// The access-policy documents received so far.
inline std::vector<std::string> restrictionDocuments()
{
    std::lock_guard<std::mutex> lock(state().mutex);
    return state().restrictions;
}

// Admissions, openings, retirements and rules pushes, in order.
inline std::vector<std::string> events()
{
    std::lock_guard<std::mutex> lock(state().mutex);
    return state().events;
}

inline void forgetRestrictions()
{
    std::lock_guard<std::mutex> lock(state().mutex);
    state().restrictions.clear();
    state().events.clear();
    state().grants.clear();
    state().refuseRules = false;
}

// Runs on each document before it is recorded, e.g. to make one slow; {} clears.
inline void onRestrictions(std::function<void(const nlohmann::json&)> hook)
{
    std::lock_guard<std::mutex> lock(state().mutex);
    state().onRestrictions = std::move(hook);
}

// Runs after each event is recorded; {} clears.
inline void onEvent(std::function<void(const std::string&)> hook)
{
    std::lock_guard<std::mutex> lock(state().mutex);
    state().onEvent = std::move(hook);
}

// Makes every rules push fail until reset.
inline void refuseRules(bool refuse)
{
    std::lock_guard<std::mutex> lock(state().mutex);
    state().refuseRules = refuse;
}

// What grant_for answers for (caller, target): "*", a JSON list, or "[]".
inline void grant(const std::string& caller, const std::string& target, const std::string& value)
{
    std::lock_guard<std::mutex> lock(state().mutex);
    state().grants[{caller, target}] = value;
}

} // namespace stand_in
