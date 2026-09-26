// Modules the runtime places in-process: the decision, and the real bundled
// capability_module and modules_state running in this test's process.
#include <gtest/gtest.h>
#include "logos_core.h"
#include "inproc_module_loader.h"
#include "token_authority.h"
#include "module_manager.h"
#include "module_registry.h"
#include "module_state_observer.h"
#include "qt_test_adapter.h"
#include "logos_protocol.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

using LogosCore::PlacementPolicy;
using LogosCore::decidePlacement;
using json = nlohmann::json;

namespace {

const json kEligible = {{"name", "x"}, {"inproc_eligible", true}};

PlacementPolicy policy(const std::string& text)
{
    PlacementPolicy parsed;
    std::string error;
    EXPECT_TRUE(LogosCore::parsePlacementPolicy(text, parsed, error)) << error;
    return parsed;
}

int setBundled(const std::string& dir)
{
    const char* dirs[] = {dir.c_str(), nullptr};
    return logos_core_set_bundled_modules_dirs(dirs);
}

bool loaded(const std::string& name)
{
    return ModuleManager::registry().isLoaded(name);
}

// The engine's principal reaches an in-process provider without a token.
json call(const char* target, const char* method, const json& args = json::array())
{
    lp_client* client = lp_client_create(target, "@runtime", nullptr, nullptr);
    if (!client) return nullptr;
    char* result = nullptr;
    char* error = nullptr;
    const int status = lp_invoke(client, method, args.dump().c_str(), 3000, &result, &error);
    json value = status == LP_OK && result ? json::parse(result, nullptr, false) : json(nullptr);
    lp_string_free(result);
    lp_string_free(error);
    lp_client_destroy(client);
    return value;
}

template <typename Predicate>
bool eventually(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return predicate();
}

} // namespace

TEST(Placement, OnlyABundledEligibleNativeModuleRunsInProcess)
{
    const PlacementPolicy inproc = policy(R"({"default":"inproc"})");
    EXPECT_TRUE(decidePlacement("m", kEligible, "native-cdylib", true, inproc).inProcess);
    EXPECT_FALSE(decidePlacement("m", kEligible, "native-cdylib", false, inproc).inProcess);
    EXPECT_FALSE(decidePlacement("m", kEligible, "qt-plugin", true, inproc).inProcess);
    const json ineligible = {{"inproc_eligible", false}, {"inproc_ineligible_reason", "go runtime"}};
    const auto refused = decidePlacement("m", ineligible, "native-cdylib", true, inproc);
    EXPECT_FALSE(refused.inProcess);
    EXPECT_EQ(refused.reason, "go runtime");
    EXPECT_FALSE(decidePlacement("m", kEligible, "native-cdylib", true, {}).inProcess)
        << "the default is a subprocess";
}

TEST(Placement, TheTablePlacesTheRuntimesModulesAndPinsCapability)
{
    const PlacementPolicy none;
    EXPECT_TRUE(decidePlacement("modules_state", kEligible, "native-cdylib", true, none).inProcess);
    const PlacementPolicy out =
        policy(R"({"modules":{"modules_state":"subprocess","capability_module":"subprocess"}})");
    EXPECT_FALSE(decidePlacement("modules_state", kEligible, "native-cdylib", true, out).inProcess);
    EXPECT_TRUE(decidePlacement("capability_module", kEligible, "native-cdylib", true, out).inProcess)
        << "no policy moves capability_module out";
    EXPECT_FALSE(decidePlacement("capability_module", kEligible, "native-cdylib", false, out).inProcess)
        << "but only a bundled one runs here";
}

TEST(Placement, ThePackageModulesNeverRunInTheRuntimesProcess)
{
    const PlacementPolicy in = policy(
        R"({"default":"inproc","modules":{"package_manager":"inproc","package_downloader":"inproc"}})");
    for (const char* name : {"package_manager", "package_downloader"}) {
        EXPECT_FALSE(decidePlacement(name, kEligible, "native-cdylib", true, {}).inProcess) << name;
        const auto moved = decidePlacement(name, kEligible, "native-cdylib", true, in);
        EXPECT_FALSE(moved.inProcess) << name << ": no policy moves it in";
        EXPECT_FALSE(moved.refused) << name;
        const auto single = decidePlacement(name, kEligible, "native-cdylib", true,
                                            policy(R"({"single_process":true})"));
        EXPECT_FALSE(single.inProcess) << name;
        EXPECT_TRUE(single.refused) << name << ": single_process cannot run it";
    }
}

TEST(Placement, SingleProcessRefusesWhatCannotRunInProcess)
{
    const PlacementPolicy single = policy(R"({"single_process":true})");
    EXPECT_TRUE(decidePlacement("m", kEligible, "native-cdylib", true, single).inProcess);
    const auto qt = decidePlacement("m", kEligible, "qt-plugin", true, single);
    EXPECT_FALSE(qt.inProcess);
    EXPECT_TRUE(qt.refused);
}

TEST(Placement, AMalformedPolicyIsRejected)
{
    PlacementPolicy parsed;
    std::string error;
    for (const char* text : {"[]", R"({"default":"elsewhere"})", R"({"modules":[]})",
                             R"({"modules":{"m":true}})", R"({"single_process":"yes"})"})
        EXPECT_FALSE(LogosCore::parsePlacementPolicy(text, parsed, error)) << text;
}

class InprocBundledTest : public ::testing::Test {
protected:
    std::string bundled;

    void SetUp() override
    {
        logos_core_terminate_all();
        logos_core_clear();
        // This case runs the real token authority, not the stand-in.
        logos::authority::detach();
        const char* dir = std::getenv("TEST_BUNDLED_MODULES_DIR");
        if (!dir || !*dir) {
            if (std::getenv("LOGOS_REQUIRE_TEST_FIXTURES")) FAIL() << "TEST_BUNDLED_MODULES_DIR not set";
            GTEST_SKIP() << "TEST_BUNDLED_MODULES_DIR not set";
        }
        bundled = dir;
    }

    void TearDown() override
    {
        logos_core_set_access_policy(nullptr);
        logos_core_terminate_all();
        logos_core_clear();
    }
};

namespace {

// The operator a token names, for core_service's resolver.
char* testOperators(const char* token, const char*, void*)
{
    return token && std::string(token) == "alice-token" ? lp_string_copy("alice") : nullptr;
}

// A client as `identity`, holding `token` for `target` (or its credential).
lp_client* clientAs(const std::string& identity, const std::string& credential,
                    const char* target = nullptr, const char* token = nullptr)
{
    lp_token_isolate_identity(identity.c_str());
    if (!credential.empty()) {
        lp_token_adopt_credential(identity.c_str(), credential.c_str());
        lp_token_save_for(identity.c_str(), "capability_module", credential.c_str());
    }
    if (target && token) lp_token_save_for(identity.c_str(), target, token);
    return lp_client_create("core_service", identity.c_str(), nullptr, nullptr);
}

json callWith(lp_client* client, const char* method, const json& args = json::array())
{
    char* result = nullptr;
    char* error = nullptr;
    const int status = lp_invoke(client, method, args.dump().c_str(), 5000, &result, &error);
    json value = status == LP_OK && result ? json::parse(result, nullptr, false) : json(nullptr);
    lp_string_free(result);
    lp_string_free(error);
    return value;
}

bool forbidden(const json& reply)
{
    return reply.is_object() && reply.value("code", std::string{}) == "FORBIDDEN";
}

struct Events {
    std::mutex mutex;
    std::vector<json> seen;
};

void onEvent(const char*, const char* data, void* userData)
{
    auto& events = *static_cast<Events*>(userData);
    std::lock_guard<std::mutex> lock(events.mutex);
    events.seen.push_back(json::parse(data ? data : "[]", nullptr, false));
}

} // namespace

// One test, because an image that ran in this process stays mapped: loading it
// again, in any later test, needs a restart.
TEST_F(InprocBundledTest, TheRuntimeRunsItsModulesInProcessBehindCoreService)
{
    ASSERT_EQ(setBundled(bundled), 0);
    ASSERT_EQ(logos_core_set_placement_policy(R"({"default":"subprocess"})"), 0);
    ASSERT_EQ(logos_core_set_shell_identity("capability_module"), -1) << "not a shell";
    ASSERT_EQ(logos_core_set_shell_identity("basecamp"), 0);
    ASSERT_EQ(logos_core_set_operator_resolver(&testOperators, nullptr), 0);
    // Reaches capability_module through its engine interface, not an RPC.
    logos_core_set_access_policy(
        R"({"version":1,"mode":"enforce","restrictions":{"modules_state":{"allowedCallers":["test_ui_plugin"]}}})");
    logos_core_start();
    EXPECT_EQ(logos_core_set_placement_policy("{}"), -1) << "protected once started";
    EXPECT_EQ(logos_core_set_shell_identity("basecamp"), -1);

    for (const char* name : {"capability_module", "modules_state"}) {
        ModuleRegistry& registry = ModuleManager::registry();
        ASSERT_TRUE(registry.isBundled(name)) << name << " at " << registry.modulePath(name);
        const auto decision = decidePlacement(name, registry.moduleMetadata(name),
                                              registry.moduleFormat(name), true,
                                              ModuleManager::placementPolicy());
        ASSERT_TRUE(decision.inProcess) << name << ": " << decision.reason;
    }
    ASSERT_TRUE(eventually([] { return loaded("capability_module") && loaded("modules_state"); }));
    for (const char* name : {"capability_module", "modules_state"}) {
        auto loader = ModuleManager::registry().loaderFor(name);
        ASSERT_TRUE(loader) << name;
        EXPECT_EQ(loader->id(), "inproc") << name;
    }
    // Reported where a client can see it.
    const json stateInfo = call("core_service", "getModuleInfo", json::array({"modules_state"}));
    EXPECT_EQ(stateInfo.value("placement", std::string{}), "inproc") << stateInfo.dump();

    // Core's feed reaches modules_state in-process, and so does this call.
    EXPECT_TRUE(eventually([] {
        const json record = call("modules_state", "module_record", json::array({"capability_module"}));
        return record.is_object() && record.value("module", std::string{}) == "capability_module";
    })) << call("modules_state", "list_modules").dump();

    // capability_module is the token authority: it minted modules_state's
    // credential, and names its holder.
    ASSERT_TRUE(logos::authority::attached());
    char* raw = logos_core_get_token("modules_state");
    const std::string credential = raw ? raw : "";
    delete[] raw;
    ASSERT_FALSE(credential.empty());
    const auto holder = logos::authority::resolveCaller(credential.c_str(), "inproc");
    ASSERT_TRUE(holder.has_value());
    EXPECT_EQ(json::parse(*holder), (json{{"kind", "module"}, {"name", "modules_state"}}));

    // core_service answers the runtime about modules, and is part of the runtime:
    // known and loaded for dependency checks, never listed.
    const json listed = call("core_service", "listModules", json::array({"all"}));
    ASSERT_TRUE(listed.is_array()) << listed.dump();
    EXPECT_NE(listed.dump().find("\"modules_state\""), std::string::npos) << listed.dump();
    EXPECT_EQ(listed.dump().find("\"core_service\""), std::string::npos) << listed.dump();
    EXPECT_TRUE(ModuleManager::registry().isLoaded("core_service"));

    // The shell: its binding, once, calling as "basecamp".
    logos_consumer* shell = logos_core_take_shell_binding();
    ASSERT_NE(shell, nullptr);
    EXPECT_EQ(logos_core_take_shell_binding(), nullptr);
    EXPECT_STREQ(logos_consumer_name(shell), "basecamp");
    auto shellCall = [&](const char* method, const json& args) {
        char* out = nullptr;
        char* err = nullptr;
        const int status = logos_consumer_call(shell, "core_service", method, args.dump().c_str(),
                                               5000, &out, &err);
        const json value = status == 0 && out ? json::parse(out, nullptr, false) : json(nullptr);
        logos_consumer_string_free(out);
        logos_consumer_string_free(err);
        return value.is_object() ? value : json::object();
    };
    // A load names how much of the graph comes too; one already loaded is ok.
    EXPECT_EQ(shellCall("loadModule", json::array({"modules_state", "module_only"}))
                  .value("status", std::string{}), "ok");
    EXPECT_EQ(shellCall("loadModule", json::array({"modules_state", "everything"}))
                  .value("code", std::string{}), "INVALID_ARGS");
    Events events;
    logos_consumer_subscription* watch =
        logos_consumer_subscribe(shell, "core_service", "moduleStateChanged", &onEvent, &events);
    ASSERT_NE(watch, nullptr);
    char* result = nullptr;
    char* error = nullptr;
    ASSERT_EQ(logos_consumer_call(shell, "core_service", "admitConsumer",
                                  R"(["test_ui_plugin","presentation"])", 5000, &result, &error),
              0) << (error ? error : "");
    const json admitted = json::parse(result ? result : "null", nullptr, false);
    logos_consumer_string_free(result);
    logos_consumer_string_free(error);
    ASSERT_EQ(admitted.value("status", std::string{}), "ok") << admitted.dump();

    // A presentation consumer reads but does not control.
    lp_client* plugin = clientAs("test_ui_plugin", admitted.value("credential", std::string{}));
    ASSERT_NE(plugin, nullptr);
    EXPECT_TRUE(callWith(plugin, "getStatus").is_object());
    EXPECT_TRUE(forbidden(callWith(plugin, "loadModule", json::array({"modules_state"}))));
    EXPECT_TRUE(forbidden(callWith(plugin, "admitConsumer", json::array({"x", "presentation"}))));
    // A pair the plugin asks capability for works until its admission ends.
    lp_client* pluginToState = lp_client_create("modules_state", "test_ui_plugin", nullptr, nullptr);
    ASSERT_NE(pluginToState, nullptr);
    auto listModules = [&](std::string* why) {
        char* out = nullptr;
        char* err = nullptr;
        const int status = lp_invoke(pluginToState, "list_modules", "[]", 5000, &out, &err);
        if (why) *why = err ? err : "";
        lp_string_free(out);
        lp_string_free(err);
        return status == LP_OK;
    };
    std::string why;
    EXPECT_TRUE(listModules(&why)) << why;
    // The access policy reached capability: a consumer it does not list gets no
    // token for modules_state.
    const json other = shellCall("admitConsumer", json::array({"other_ui", "presentation"}));
    ASSERT_EQ(other.value("status", std::string{}), "ok") << other.dump();
    lp_client* otherUi = clientAs("other_ui", other.value("credential", std::string{}));
    lp_client* otherToState = lp_client_create("modules_state", "other_ui", nullptr, nullptr);
    ASSERT_NE(otherToState, nullptr);
    {
        char* out = nullptr;
        char* err = nullptr;
        EXPECT_NE(lp_invoke(otherToState, "list_modules", "[]", 5000, &out, &err), LP_OK)
            << "modules_state took a caller the policy does not list";
        lp_string_free(out);
        lp_string_free(err);
    }
    lp_client_destroy(otherToState);
    lp_client_destroy(otherUi);
    // The shell retires only what it admitted, never a module.
    EXPECT_EQ(shellCall("retireConsumer", json::array({"modules_state"})).value("code", std::string{}),
              "NOT_FOUND");
    EXPECT_TRUE(logos::authority::resolveCaller(credential.c_str(), "inproc").has_value());

    // An operator the embedder names forwards calls, but never to the token store.
    lp_client* alice = clientAs("alice-cli", {}, "core_service", "alice-token");
    ASSERT_NE(alice, nullptr);
    EXPECT_TRUE(callWith(alice, "listModules", json::array({"loaded"})).is_array());
    const json refused = callWith(alice, "callModuleMethod",
                                  json::array({"capability_module", "requestModule",
                                               json::array({"", "modules_state"})}));
    EXPECT_EQ(refused.value("code", std::string{}), "METHOD_FAILED") << refused.dump();
    EXPECT_EQ(refused.value("error", json::object()).value("code", std::string{}), "unauthorized");
    EXPECT_EQ(callWith(alice, "watchModuleEvents", json::array({"capability_module", ""})),
              json(false));
    const json forwarded = callWith(alice, "callModuleMethod",
                                    json::array({"modules_state", "list_modules", json::array()}));
    EXPECT_EQ(forwarded.value("status", std::string{}), "ok") << forwarded.dump();
    EXPECT_TRUE(forbidden(callWith(alice, "admitConsumer", json::array({"y", "presentation"}))));
    // An unknown token gets nothing.
    lp_client* stranger = clientAs("stranger-cli", {}, "core_service", "no-such-token");
    EXPECT_TRUE(callWith(stranger, "listModules").is_null());

    // The peering scope: peering_module's question alone, which capability decides
    // from the remote policy the runtime hands it.
    const std::string peeringCredential = logos::authority::admit("peering_module", "module");
    ASSERT_FALSE(peeringCredential.empty());
    lp_client* peering = clientAs("peering_module", peeringCredential);
    const json question = json::array({"peer-1", "wallet", "modules_state"});
    const json unlisted = callWith(peering, "evaluateRemoteAccess", question);
    EXPECT_EQ(unlisted.value("allow", true), false) << unlisted.dump();
    ASSERT_TRUE(logos::authority::setRemotePolicy(R"({"peer-1/wallet":["modules_state"]})"));
    const json granted = callWith(peering, "evaluateRemoteAccess", question);
    EXPECT_EQ(granted.value("allow", false), true) << granted.dump();
    EXPECT_FALSE(granted.value("decision", std::string{}).empty());
    EXPECT_TRUE(forbidden(callWith(alice, "evaluateRemoteAccess", question)));
    ASSERT_TRUE(logos::authority::setRemotePolicy("{}"));
    // A facade's scope: it gets no token for anything but peering_module.
    const std::string facadeCredential = logos::authority::admit("an_import", "module");
    ASSERT_TRUE(logos::authority::setCallerScopes(R"({"an_import":["peering_module"]})"));
    lp_client* facade = clientAs("an_import", facadeCredential);
    lp_client* facadeToState = lp_client_create("modules_state", "an_import", nullptr, nullptr);
    {
        char* out = nullptr;
        char* err = nullptr;
        EXPECT_NE(lp_invoke(facadeToState, "list_modules", "[]", 5000, &out, &err), LP_OK)
            << "a facade reached a module outside its scope";
        lp_string_free(out);
        lp_string_free(err);
    }
    lp_client_destroy(facadeToState);
    lp_client_destroy(facade);
    lp_client_destroy(peering);
    ASSERT_TRUE(logos::authority::setCallerScopes("{}"));
    logos::authority::retire("an_import");
    logos::authority::retire("peering_module");

    // Detector: each watch left a forwarder behind, so the Nth watcher saw every event N times.
    Events relayed;
    lp_subscription* moduleEvents = lp_subscribe(alice, "module_event", &onEvent, &relayed);
    ASSERT_NE(moduleEvents, nullptr);
    for (const char* event : {"module_state_changed", "module_state_changed", ""})
        EXPECT_EQ(callWith(alice, "watchModuleEvents", json::array({"modules_state", event})), json(true))
            << "'" << event << "'";
    // modules_state emits module_state_changed for each transition core feeds it.
    auto probe = [] {
        logos::ModuleStateObserver::instance().record("watch_probe", logos::module_state::kAbsent,
                                               logos::module_state::kLoading);
        logos::ModuleStateObserver::instance().flush();
    };
    auto probes = [&] {
        std::lock_guard<std::mutex> lock(relayed.mutex);
        return std::count_if(relayed.seen.begin(), relayed.seen.end(), [](const json& e) {
            return e.is_array() && e.size() >= 3 && e[0] == "modules_state" && e[2] == "watch_probe";
        });
    };
    auto settle = [] { std::this_thread::sleep_for(std::chrono::milliseconds(500)); };
    probe();
    EXPECT_TRUE(eventually([&] { return probes() >= 1; }));
    settle();
    EXPECT_EQ(probes(), 1) << "one forwarder however often the watch is asked for";
    // Its module leaving ends the watch, and a new one forwards once.
    logos::ModuleStateObserver::instance().record("modules_state", logos::module_state::kReady,
                                           logos::module_state::kError, std::nullopt, std::nullopt,
                                           std::string("watch test"));
    logos::ModuleStateObserver::instance().flush();
    probe();
    settle();
    EXPECT_EQ(probes(), 1) << "a watch outlived its module";
    EXPECT_EQ(callWith(alice, "watchModuleEvents",
                       json::array({"modules_state", "module_state_changed"})), json(true));
    probe();
    EXPECT_TRUE(eventually([&] { return probes() >= 2; }));
    settle();
    EXPECT_EQ(probes(), 2);
    lp_unsubscribe(moduleEvents);

    lp_client_destroy(plugin);
    lp_client_destroy(alice);
    lp_client_destroy(stranger);
    const std::string pluginCredential = admitted.value("credential", std::string{});
    EXPECT_EQ(shellCall("retireConsumer", json::array({"test_ui_plugin"})).value("status", std::string{}),
              "ok");
    EXPECT_FALSE(logos::authority::resolveCaller(pluginCredential.c_str(), "inproc").has_value());
    // Its cached pair is refused at modules_state too: capability revoked it
    // there, and the plugin can no longer mint another.
    EXPECT_TRUE(eventually([&] { return !listModules(nullptr); }))
        << "modules_state still takes the retired plugin's pair token";
    lp_client_destroy(pluginToState);

    EXPECT_EQ(logos_core_unload_module("modules_state", false), 1);
    EXPECT_FALSE(loaded("modules_state"));
    EXPECT_TRUE(call("modules_state", "list_modules").is_null()) << "its provider is withdrawn";
    EXPECT_FALSE(logos::authority::resolveCaller(credential.c_str(), "inproc").has_value())
        << "unloading retires the admission";
    EXPECT_NE(logos_core_load_module("modules_state", LOGOS_LOAD_MODULE_ONLY), 1)
        << "its image is still mapped";

    // The shell saw it go, through core_service.
    EXPECT_TRUE(eventually([&] {
        std::lock_guard<std::mutex> lock(events.mutex);
        for (const json& e : events.seen)
            if (e.is_array() && e.size() >= 3 && e[0] == "modules_state" && e[2] == "unloaded")
                return true;
        return false;
    }));
    logos_consumer_unsubscribe(watch);
    logos_consumer_release(shell);
}
