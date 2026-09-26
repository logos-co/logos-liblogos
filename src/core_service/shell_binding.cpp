#include "shell_binding.h"
#include "embedded_core_service.h"

#include "token_authority.h"
#include "logos_core.h"

#include <logos_protocol.h>
#include <spdlog/spdlog.h>

#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>

// Thin wrappers over this library's own runtime, so an app that also links the
// Qt runtime can never hand one runtime's handles to the other.
struct logos_consumer {
    std::string name;
    std::string credential;
    std::mutex mutex;
    std::map<std::string, lp_client*> clients;
    std::set<lp_subscription*> subscriptions;
    bool released = false;
};

struct logos_consumer_subscription {
    logos_consumer* consumer = nullptr;
    lp_subscription* subscription = nullptr;
};

namespace {

struct Binding {
    std::mutex mutex;
    logos_consumer* consumer = nullptr;
    bool taken = false;
};

Binding& binding()
{
    static Binding value;
    return value;
}

lp_client* clientFor(logos_consumer* consumer, const std::string& target)
{
    std::lock_guard<std::mutex> lock(consumer->mutex);
    if (consumer->released) return nullptr;
    lp_client*& slot = consumer->clients[target];
    if (!slot) slot = lp_client_create(target.c_str(), consumer->name.c_str(), nullptr, nullptr);
    return slot;
}

void closeAll(logos_consumer* consumer)
{
    std::map<std::string, lp_client*> clients;
    std::set<lp_subscription*> subscriptions;
    {
        std::lock_guard<std::mutex> lock(consumer->mutex);
        consumer->released = true;
        clients.swap(consumer->clients);
        subscriptions.swap(consumer->subscriptions);
    }
    for (lp_subscription* subscription : subscriptions) lp_unsubscribe(subscription);
    for (auto& [target, client] : clients)
        if (client) lp_client_destroy(client);
}

} // namespace

namespace logos::shell_binding {

bool prepare()
{
    const std::string name = core_service::shellIdentity();
    if (name.empty()) return true;
    if (!authority::attached()) {
        spdlog::error("shell '{}': no token authority is running, so it has no identity", name);
        return false;
    }
    const std::string credential = authority::admit(name, "shell");
    if (credential.empty()
        || lp_token_isolate_identity(name.c_str()) != LP_OK
        || lp_token_adopt_credential(name.c_str(), credential.c_str()) != LP_OK
        || lp_token_save_for(name.c_str(), "capability_module", credential.c_str()) != LP_OK) {
        spdlog::error("shell '{}' could not be admitted", name);
        authority::retire(name);
        return false;
    }
    auto* consumer = new logos_consumer;
    consumer->name = name;
    consumer->credential = credential;
    std::lock_guard<std::mutex> lock(binding().mutex);
    binding().consumer = consumer;
    binding().taken = false;
    return true;
}

void shutdown()
{
    logos_consumer* consumer = nullptr;
    {
        std::lock_guard<std::mutex> lock(binding().mutex);
        consumer = binding().consumer;
        binding().consumer = nullptr;
        binding().taken = false;
    }
    if (!consumer) return;
    closeAll(consumer);
    authority::retire(consumer->name);
    delete consumer;
}

} // namespace logos::shell_binding

extern "C" {

logos_consumer* logos_core_take_shell_binding(void)
{
    std::lock_guard<std::mutex> lock(binding().mutex);
    if (!binding().consumer || binding().taken) return nullptr;
    binding().taken = true;
    return binding().consumer;
}

const char* logos_consumer_name(const logos_consumer* consumer)
{
    return consumer ? consumer->name.c_str() : nullptr;
}

char* logos_consumer_credential(const logos_consumer* consumer)
{
    return consumer ? lp_string_copy(consumer->credential.c_str()) : nullptr;
}

int logos_consumer_call(logos_consumer* consumer, const char* target, const char* method,
                        const char* args_json, int timeout_ms, char** out_result_json,
                        char** out_error_json)
{
    if (!consumer || !target || !method) return LP_ERR_INVALID_ARG;
    lp_client* client = clientFor(consumer, target);
    if (!client) return LP_ERR_UNAVAILABLE;
    return lp_invoke(client, method, args_json ? args_json : "[]", timeout_ms,
                     out_result_json, out_error_json);
}

int logos_consumer_call_async(logos_consumer* consumer, const char* target, const char* method,
                              const char* args_json, int timeout_ms,
                              logos_consumer_result_cb cb, void* user_data)
{
    if (!consumer || !target || !method || !cb) return LP_ERR_INVALID_ARG;
    lp_client* client = clientFor(consumer, target);
    if (!client) return LP_ERR_UNAVAILABLE;
    return lp_invoke_async(client, method, args_json ? args_json : "[]", timeout_ms, cb,
                           user_data);
}

logos_consumer_subscription* logos_consumer_subscribe(logos_consumer* consumer,
                                                      const char* target,
                                                      const char* event_name,
                                                      logos_consumer_event_cb cb,
                                                      void* user_data)
{
    if (!consumer || !target || !cb) return nullptr;
    lp_client* client = clientFor(consumer, target);
    if (!client) return nullptr;
    lp_subscription* subscription = lp_subscribe(client, event_name ? event_name : "", cb, user_data);
    if (!subscription) return nullptr;
    {
        std::lock_guard<std::mutex> lock(consumer->mutex);
        consumer->subscriptions.insert(subscription);
    }
    return new logos_consumer_subscription{consumer, subscription};
}

void logos_consumer_unsubscribe(logos_consumer_subscription* subscription)
{
    if (!subscription) return;
    bool owned = false;
    {
        std::lock_guard<std::mutex> lock(subscription->consumer->mutex);
        owned = subscription->consumer->subscriptions.erase(subscription->subscription) > 0;
    }
    if (owned) lp_unsubscribe(subscription->subscription);
    delete subscription;
}

void logos_consumer_string_free(char* value)
{
    lp_string_free(value);
}

// The identity stays admitted until cleanup; only this handle's calls end.
void logos_consumer_release(logos_consumer* consumer)
{
    if (consumer) closeAll(consumer);
}

} // extern "C"
