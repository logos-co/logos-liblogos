// The runtime and its token authority, capability_module: nothing but the
// authority loads without one, the authority runs in-process only and never
// unloads, and the access policy reaches it whole, on every change.

#include "fake_module_host.h"
#include "logos_core.h"
#include "token_authority.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace {

class TokenAuthorityTest : public FakeHostFixture {
protected:
    void SetUp() override
    {
        FakeHostFixture::SetUp();
        useFakeHost();
        stand_in::forgetRestrictions();
    }

    void TearDown() override
    {
        // clear() drops the policy and re-attaches the stand-in for the next case.
        FakeHostFixture::TearDown();
    }
};

// Where `event` first happened in the stand-in's log, or -1.
long indexOf(const std::string& event)
{
    const auto events = stand_in::events();
    const auto it = std::find(events.begin(), events.end(), event);
    return it == events.end() ? -1 : static_cast<long>(it - events.begin());
}

char* aliceOperator(const char* token, const char*, void*)
{
    return token && std::string(token) == "alice-token" ? lp_string_copy("alice") : nullptr;
}

// callModuleMethod on core_service as operator alice.
json forwardAsAlice(const char* module, const char* method, const json& args = json::array())
{
    lp_token_isolate_identity("@test-alice");
    lp_token_save_for("@test-alice", "core_service", "alice-token");
    lp_client* client = lp_client_create("core_service", "@test-alice", nullptr, nullptr);
    if (!client) return nullptr;
    char* result = nullptr;
    char* error = nullptr;
    const json call = json::array({module, method, args});
    const int status = lp_invoke(client, "callModuleMethod", call.dump().c_str(), 10000, &result, &error);
    json value = status == LP_OK && result ? json::parse(result, nullptr, false) : json(nullptr);
    lp_string_free(result);
    lp_string_free(error);
    lp_client_destroy(client);
    return value;
}

bool refusedByGrant(const json& reply)
{
    if (!reply.is_object()) return false;
    if (reply.value("code", std::string{}) == "FORBIDDEN") return true;
    const json error = reply.value("error", json::object());
    return error.value("code", std::string{}) == "not_authorised"
        && error.value("origin", std::string{}) == "core_service";
}

long firstRulesPush()
{
    const auto events = stand_in::events();
    for (size_t i = 0; i < events.size(); ++i)
        if (events[i].rfind("set_", 0) == 0) return static_cast<long>(i);
    return -1;
}

} // namespace

// Detector: without an authority, core minted the credential itself.
TEST_F(TokenAuthorityTest, ALoadWithoutAnAuthorityIsRefusedAndSpawnsNothing)
{
    plantModule("orphan", "report-ok");
    logos::authority::detach();

    EXPECT_EQ(logos_core_load_module("orphan", LOGOS_LOAD_MODULE_ONLY), 0);
    EXPECT_FALSE(logos_core_is_module_loaded("orphan"));
    EXPECT_EQ(spawnCount("orphan"), 0) << hostEventLog();
    EXPECT_NE(reasonFor("orphan", logos::module_state::kError).find("no token authority"),
              std::string::npos);
}

// Detector: a start without capability_module published core_service and a shell
// binding on credentials core had minted.
TEST_F(TokenAuthorityTest, StartWithoutCapabilityPublishesNothing)
{
    logos::authority::detach();
    ASSERT_EQ(logos_core_set_shell_identity("test_shell"), 0);
    logos_core_start();

    EXPECT_FALSE(logos::authority::attached());
    EXPECT_EQ(logos_core_take_shell_binding(), nullptr);
    EXPECT_FALSE(ModuleManager::registry().isLoaded("core_service"));
}

// Its image stays mapped and nothing is admitted without it, so only the
// runtime's teardown stops it.
TEST_F(TokenAuthorityTest, TheAuthorityCannotBeUnloaded)
{
    logos_core_mark_module_loaded("capability_module");

    EXPECT_EQ(logos_core_unload_module("capability_module", false), 0);
    EXPECT_EQ(logos_core_unload_module("capability_module", true), 0);
    EXPECT_TRUE(logos_core_is_module_loaded("capability_module"));
}

// Detector: a capability_module that could only be hosted ran as a subprocess,
// where it cannot hand the runtime its engine.
TEST_F(TokenAuthorityTest, AHostedOnlyCapabilityIsRefused)
{
    plantModule("capability_module", "report-ok");

    EXPECT_EQ(logos_core_load_module("capability_module", LOGOS_LOAD_MODULE_ONLY), 0);
    EXPECT_FALSE(logos_core_is_module_loaded("capability_module"));
    EXPECT_EQ(spawnCount("capability_module"), 0) << hostEventLog();
    EXPECT_NE(reasonFor("capability_module", logos::module_state::kError).find("in-process"),
              std::string::npos);
}

// Detector: the policy reached capability_module one target at a time, over an
// RPC that it no longer serves.
TEST_F(TokenAuthorityTest, EachLoadAndUnloadSendsTheWholePolicy)
{
    logos_core_set_access_policy(
        R"({"version":1,"mode":"enforce","restrictions":{"not_loaded":{"allowedCallers":["someone"]}}})");
    plantModule("healthy", "report-ok");

    ASSERT_EQ(logos_core_load_module("healthy", LOGOS_LOAD_MODULE_ONLY), 1);
    auto documents = stand_in::restrictionDocuments();
    ASSERT_EQ(documents.size(), 1u);
    const json loaded = json::parse(documents[0]);
    EXPECT_EQ(loaded.value("not_loaded", json()), json::array({"someone", "@op:*"})) << loaded.dump();
    EXPECT_TRUE(loaded.contains("healthy")) << loaded.dump();

    ASSERT_EQ(logos_core_unload_module("healthy", false), 1);
    documents = stand_in::restrictionDocuments();
    ASSERT_EQ(documents.size(), 2u);
    const json unloaded = json::parse(documents[1]);
    EXPECT_FALSE(unloaded.contains("healthy")) << unloaded.dump();
    EXPECT_TRUE(unloaded.contains("not_loaded")) << unloaded.dump();
}

// Detector: a rule with no callers was dropped from the document, and a target
// absent from it is open to everyone. Version 1 leaves operators unrestricted.
TEST_F(TokenAuthorityTest, ARuleWithNoCallersIsSentNotDropped)
{
    ASSERT_EQ(logos_core_set_access_policy(
        R"({"version":1,"mode":"enforce","restrictions":{"quiet":{},"hushed":{"allowedCallers":[]}}})"), 0);
    plantModule("healthy", "report-ok");

    ASSERT_EQ(logos_core_load_module("healthy", LOGOS_LOAD_MODULE_ONLY), 1);
    const auto documents = stand_in::restrictionDocuments();
    ASSERT_FALSE(documents.empty());
    const json document = json::parse(documents.back());
    EXPECT_EQ(document.value("quiet", json()), json::array({"@op:*"})) << document.dump();
    EXPECT_EQ(document.value("hushed", json()), json::array({"@op:*"})) << document.dump();
}

// Detector: a module that died left its routes in capability_module's rules
// until something else loaded or unloaded.
TEST_F(TokenAuthorityTest, ACrashSendsThePolicyAgain)
{
    ASSERT_EQ(logos_core_set_access_policy(R"({"version":1,"mode":"enforce","restrictions":{}})"), 0);
    plantModule("doomed", "report-ok");
    ASSERT_EQ(logos_core_load_module("doomed", LOGOS_LOAD_MODULE_ONLY), 1);
    ASSERT_FALSE(stand_in::restrictionDocuments().empty());
    ASSERT_TRUE(json::parse(stand_in::restrictionDocuments().back()).contains("doomed"));
    const auto pids = ModuleManager::getModuleProcessIds();
    ASSERT_TRUE(pids.count("doomed")) << "no pid for doomed";

    ASSERT_TRUE(logos_test::killPid(pids.at("doomed")));
    const auto stillListed = [] {
        return json::parse(stand_in::restrictionDocuments().back()).contains("doomed");
    };
    for (int i = 0; i < 500 && stillListed(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(logos_core_is_module_loaded("doomed"));
    EXPECT_FALSE(stillListed()) << stand_in::restrictionDocuments().back();
}

// Detector: a module was admitted before any rule covered it, and callers could
// pair with it while it was still loading.
TEST_F(TokenAuthorityTest, AModuleIsAdmittedPendingUnderItsRulesAndOpenedOnceCommitted)
{
    ASSERT_EQ(logos_core_set_access_policy(R"({"version":1,"mode":"enforce","restrictions":{}})"), 0);
    plantModule("healthy", "report-ok");
    bool loadedWhenOpened = false;
    stand_in::onEvent([&](const std::string& event) {
        if (event == "open:healthy") loadedWhenOpened = logos_core_is_module_loaded("healthy");
    });
    ASSERT_EQ(logos_core_load_module("healthy", LOGOS_LOAD_MODULE_ONLY), 1);
    stand_in::onEvent({});

    const long rules = firstRulesPush();
    const long pending = indexOf("admit_pending:healthy");
    const long opened = indexOf("open:healthy");
    ASSERT_GE(rules, 0);
    ASSERT_GT(pending, rules) << "admitted before its rules were pushed";
    ASSERT_GT(opened, pending);
    EXPECT_EQ(indexOf("admit:healthy"), -1) << "admitted open";
    EXPECT_TRUE(json::parse(stand_in::restrictionDocuments().front()).contains("healthy"));
    EXPECT_TRUE(loadedWhenOpened) << "opened to callers before its load committed";
}

TEST_F(TokenAuthorityTest, AFailedLoadIsRetiredAndLeavesTheRules)
{
    ASSERT_EQ(logos_core_set_access_policy(R"({"version":1,"mode":"enforce","restrictions":{}})"), 0);
    plantModule("broken", "report-fail");

    EXPECT_EQ(logos_core_load_module("broken", LOGOS_LOAD_MODULE_ONLY), 0);
    EXPECT_GE(indexOf("admit_pending:broken"), 0);
    EXPECT_GE(indexOf("retire:broken"), 0);
    EXPECT_EQ(indexOf("open:broken"), -1);
    const auto documents = stand_in::restrictionDocuments();
    ASSERT_GE(documents.size(), 2u);
    EXPECT_TRUE(json::parse(documents.front()).contains("broken"));
    EXPECT_FALSE(json::parse(documents.back()).contains("broken")) << documents.back();
}

// Detector: a refused push was logged and the load went on, unchecked.
TEST_F(TokenAuthorityTest, ARefusedRulesPushFailsTheLoad)
{
    plantModule("unruled", "report-ok");
    stand_in::refuseRules(true);
    EXPECT_EQ(logos_core_load_module("unruled", LOGOS_LOAD_MODULE_ONLY), 0);
    stand_in::refuseRules(false);

    EXPECT_FALSE(logos_core_is_module_loaded("unruled"));
    EXPECT_EQ(indexOf("admit_pending:unruled"), -1);
    EXPECT_EQ(indexOf("admit:unruled"), -1);
    EXPECT_NE(reasonFor("unruled", logos::module_state::kError).find("access policy"),
              std::string::npos) << reasonFor("unruled", logos::module_state::kError);
}

// Detector: each load's rules named only the modules already committed, so two
// loads of one module's dependents dropped each other from its rule.
TEST_F(TokenAuthorityTest, ConcurrentLoadsKeepEachOtherInTheRules)
{
    ASSERT_EQ(logos_core_set_access_policy(R"({"version":1,"mode":"enforce","restrictions":{}})"), 0);
    plantModule("shared_dep", "report-ok");
    plantModule("first_caller", "slow-ok");
    plantModule("second_caller", "slow-ok");
    const char* deps[] = {"shared_dep"};
    logos_core_register_module_dependencies("first_caller", deps, 1);
    logos_core_register_module_dependencies("second_caller", deps, 1);
    ASSERT_EQ(logos_core_load_module("shared_dep", LOGOS_LOAD_MODULE_ONLY), 1);

    std::thread first([] { EXPECT_EQ(logos_core_load_module("first_caller", LOGOS_LOAD_MODULE_ONLY), 1); });
    std::thread second([] { EXPECT_EQ(logos_core_load_module("second_caller", LOGOS_LOAD_MODULE_ONLY), 1); });
    first.join();
    second.join();

    // Once a caller is in shared_dep's rule it stays there: loading counts as loaded.
    const char* callers[] = {"first_caller", "second_caller"};
    bool seen[] = {false, false};
    for (const auto& text : stand_in::restrictionDocuments()) {
        const json rule = json::parse(text).value("shared_dep", json::array());
        for (int i = 0; i < 2; ++i) {
            const bool listed = std::find(rule.begin(), rule.end(), callers[i]) != rule.end();
            if (seen[i]) EXPECT_TRUE(listed) << callers[i] << " dropped from " << rule.dump();
            seen[i] = seen[i] || listed;
        }
    }
    EXPECT_TRUE(seen[0] && seen[1]) << hostEventLog();
}

// An older capability_module hands over a version 1 table: its modules are admitted
// open, as before, since it has no pending admissions.
TEST_F(TokenAuthorityTest, TheAuthorityMayBeAVersionOneEngine)
{
    logos::authority::detach();
    ASSERT_TRUE(logos::authority::attach(&stand_in::engineVersion1(), nullptr));
    EXPECT_FALSE(logos::authority::isVersion2());
    plantModule("old_engine_client", "report-ok");

    ASSERT_EQ(logos_core_load_module("old_engine_client", LOGOS_LOAD_MODULE_ONLY), 1);
    EXPECT_GE(indexOf("admit:old_engine_client"), 0);
    EXPECT_EQ(indexOf("admit_pending:old_engine_client"), -1);
}

// Version 2 rules reach capability as written: the shell gets only what a rule
// gives it, and operators only their "@op:" entries. The runtime adds only
// core_service on the package modules. Derived rules keep admitting operators.
TEST_F(TokenAuthorityTest, VersionTwoRulesGoAsWritten)
{
    ASSERT_EQ(logos_core_set_shell_identity("basecamp"), 0);
    ASSERT_EQ(logos_core_set_access_policy(R"({"version":2,"mode":"enforce","restrictions":{
        "keyed":{"allowedCallers":{"ui":["m1"],"@op:alice":"*"}},
        "listed":{"allowedCallers":["x"]},
        "closed":{},
        "package_manager":{"allowedCallers":{"package_manager_ui":"*"}},
        "package_downloader":{"allowedCallers":[]}}})"), 0);
    plantModule("healthy", "report-ok");
    ASSERT_EQ(logos_core_load_module("healthy", LOGOS_LOAD_MODULE_ONLY), 1);

    EXPECT_EQ(indexOf("set_restrictions"), -1);
    const json document = json::parse(stand_in::restrictionDocuments().back());
    EXPECT_EQ(document.value("keyed", json()), json::parse(R"({"ui":["m1"],"@op:alice":"*"})"));
    EXPECT_EQ(document.value("listed", json()), json::array({"x"}));
    EXPECT_EQ(document.value("closed", json()), json::array());
    EXPECT_EQ(document.value("package_manager", json()),
              json::parse(R"({"package_manager_ui":"*","core_service":"*"})"));
    EXPECT_EQ(document.value("package_downloader", json()), json::array({"core_service"}));
    const json derived = document.value("healthy", json::array());
    for (const char* caller : {"core", "core_service", "basecamp", "@op:*"})
        EXPECT_NE(std::find(derived.begin(), derived.end(), caller), derived.end())
            << caller << " missing from " << derived.dump();
}

TEST_F(TokenAuthorityTest, ExplicitModeSendsOnlyTheRulesWritten)
{
    ASSERT_EQ(logos_core_set_access_policy(
        R"({"version":1,"mode":"explicit","restrictions":{"listed":{"allowedCallers":["x"]}}})"), 0);
    plantModule("healthy", "report-ok");
    ASSERT_EQ(logos_core_load_module("healthy", LOGOS_LOAD_MODULE_ONLY), 1);

    EXPECT_EQ(json::parse(stand_in::restrictionDocuments().back()),
              json::parse(R"({"listed":["x","@op:*"]})"));
}

// Detector: under a version 1 rule on a package module, operators' package commands
// (which arrive as core_service) were refused, though version 1 leaves operators free.
TEST_F(TokenAuthorityTest, VersionOnePackageRulesAdmitCoreService)
{
    ASSERT_EQ(logos_core_set_shell_identity("basecamp"), 0);
    ASSERT_EQ(logos_core_set_access_policy(R"({"version":1,"mode":"explicit","restrictions":{
        "package_manager":{"allowedCallers":["package_manager_ui"]},
        "package_downloader":{"allowedCallers":[]},
        "listed":{"allowedCallers":["x"]}}})"), 0);
    plantModule("healthy", "report-ok");
    ASSERT_EQ(logos_core_load_module("healthy", LOGOS_LOAD_MODULE_ONLY), 1);

    EXPECT_EQ(json::parse(stand_in::restrictionDocuments().back()), json::parse(R"({
        "package_manager":["package_manager_ui","basecamp","core_service","@op:*"],
        "package_downloader":["basecamp","core_service","@op:*"],
        "listed":["x","basecamp","@op:*"]})"));
}

// Detector for the fallback: an older capability_module cannot bind operators or
// grant methods, so it gets version 1 rules as before, and a version 2 policy
// is refused rather than half-enforced.
TEST_F(TokenAuthorityTest, AVersionOneEngineTakesOnlyVersionOnePolicies)
{
    logos::authority::detach();
    ASSERT_TRUE(logos::authority::attach(&stand_in::engineVersion1(), nullptr));
    ASSERT_EQ(logos_core_set_access_policy(
        R"({"version":1,"mode":"enforce","restrictions":{"listed":{"allowedCallers":["x"]}}})"), 0);
    plantModule("healthy", "report-ok");
    ASSERT_EQ(logos_core_load_module("healthy", LOGOS_LOAD_MODULE_ONLY), 1);
    EXPECT_GE(indexOf("set_restrictions"), 0);
    EXPECT_EQ(indexOf("set_access_rules"), -1);
    EXPECT_EQ(json::parse(stand_in::restrictionDocuments().back()).value("listed", json()),
              json::array({"x"}));

    ASSERT_EQ(logos_core_set_access_policy(
        R"({"version":2,"mode":"explicit","restrictions":{"listed":{"allowedCallers":["x"]}}})"), 0);
    plantModule("second", "report-ok");
    EXPECT_EQ(logos_core_load_module("second", LOGOS_LOAD_MODULE_ONLY), 0);
    EXPECT_NE(reasonFor("second", logos::module_state::kError).find("access policy"),
              std::string::npos);
}

// Detector: operators reach the package modules as core_service, so core_service
// forwarded any method, whatever the operator's own grant said.
TEST_F(TokenAuthorityTest, AnOperatorsPackageCallsKeepToItsGrant)
{
    ASSERT_EQ(logos_core_set_operator_resolver(&aliceOperator, nullptr), 0);
    logos_core_register_module("package_manager", "/fake/package_manager");
    logos_core_register_module("package_downloader", "/fake/package_downloader");
    logos_core_start();
    ASSERT_TRUE(ModuleManager::registry().isLoaded("core_service"));
    logos_core_mark_module_loaded("package_manager");
    logos_core_mark_module_loaded("package_downloader");
    // package_manager, served here and holding a token core_service presents.
    lp_provider* packages = lp_provider_create("package_manager", R"([{"protocol":"inproc"}])");
    ASSERT_NE(packages, nullptr);
    ASSERT_EQ(lp_provider_save_token(packages, "core_service", "pm-token"), LP_OK);
    ASSERT_EQ(lp_token_save_for("core_service", "package_manager", "pm-token"), LP_OK);
    ASSERT_EQ(lp_provider_register(packages, [](const char*, const char*, void*) {
        return lp_string_copy("\"served\""); }, [](void*) { return lp_string_copy("[]"); },
        nullptr, nullptr), LP_OK);
    stand_in::grant("@op:alice", "package_manager", R"(["listKeys"])");
    stand_in::grant("@op:alice", "package_downloader", "[]");

    const json outside = forwardAsAlice("package_manager", "installPlugin", {"x.lgx"});
    EXPECT_EQ(outside.value("code", std::string{}), "METHOD_FAILED") << outside.dump();
    EXPECT_TRUE(refusedByGrant(outside)) << outside.dump();
    for (const char* method : {"listKeys", "name"}) {
        const json served = forwardAsAlice("package_manager", method);
        EXPECT_EQ(served.value("result", json()), "served") << method << ": " << served.dump();
    }
    const json none = forwardAsAlice("package_downloader", "listPackages");
    EXPECT_EQ(none.value("code", std::string{}), "FORBIDDEN") << none.dump();
    lp_provider_destroy(packages);
}
