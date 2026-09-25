// A stand-in for capability_module's engine interface.
//
// The real capability_module can run in-process only once per process, and this
// test binary runs every case in one. So every case is admitted by this
// instead: it mints a credential per admission, names the callers it admitted,
// grants operator pairs, and records every access-policy document the runtime
// sends. InprocBundledTest detaches it and runs the real one.
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

struct State {
    std::mutex mutex;
    std::map<std::string, std::pair<std::string, unsigned long long>> admitted;  // name -> (credential, generation)
    std::map<std::string, std::string> names;                                   // credential -> name
    std::vector<std::string> restrictions;                                       // every document, in order
    std::function<void(const nlohmann::json&)> onRestrictions;                   // runs before one is recorded
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

inline char* admit(const char* name, const char* kind, unsigned long long* generation)
{
    const std::string k = kind ? kind : "";
    if (!name || !*name || (k != "module" && k != "shell" && k != "presentation")) return nullptr;
    std::lock_guard<std::mutex> lock(state().mutex);
    State& s = state();
    if (auto it = s.admitted.find(name); it != s.admitted.end()) s.names.erase(it->second.first);
    const unsigned long long g = s.next++;
    const std::string credential = "stand-in-" + std::string(name) + "-" + std::to_string(g);
    s.admitted[name] = {credential, g};
    s.names[credential] = name;
    if (generation) *generation = g;
    return copy(credential);
}

inline int retire(const char* name, unsigned long long generation)
{
    std::lock_guard<std::mutex> lock(state().mutex);
    State& s = state();
    auto it = name ? s.admitted.find(name) : s.admitted.end();
    if (it == s.admitted.end() || it->second.second != generation) return -1;
    s.names.erase(it->second.first);
    s.admitted.erase(it);
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
    return it == state().admitted.end() ? nullptr : copy(it->second.first);
}

inline char* grantOperatorPair(const char* op, const char* target)
{
    std::lock_guard<std::mutex> lock(state().mutex);
    if (!op || !target || !state().admitted.count(target)) return nullptr;
    return copy("stand-in-op-" + std::string(op) + "-" + target);
}

inline int setRestrictions(const char* document)
{
    const nlohmann::json parsed = nlohmann::json::parse(document ? document : "", nullptr, false);
    if (!parsed.is_object()) return -1;
    std::function<void(const nlohmann::json&)> hook;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        hook = state().onRestrictions;
    }
    if (hook) hook(parsed);
    std::lock_guard<std::mutex> lock(state().mutex);
    state().restrictions.push_back(parsed.dump());
    return 0;
}

inline void stringFree(char* value) { std::free(value); }

inline const logos_capability_engine_v1& engine()
{
    static const logos_capability_engine_v1 table = {
        sizeof(logos_capability_engine_v1), LOGOS_CAPABILITY_ENGINE_VERSION,
        &admit, &retire, &resolveCaller, &credentialFor, &grantOperatorPair,
        &setRestrictions, &stringFree,
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

inline void forgetRestrictions()
{
    std::lock_guard<std::mutex> lock(state().mutex);
    state().restrictions.clear();
}

// Runs on each document before it is recorded, e.g. to make one slow; {} clears.
inline void onRestrictions(std::function<void(const nlohmann::json&)> hook)
{
    std::lock_guard<std::mutex> lock(state().mutex);
    state().onRestrictions = std::move(hook);
}

} // namespace stand_in
