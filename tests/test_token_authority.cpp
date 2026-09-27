// The runtime and its token authority, capability_module: nothing but the
// authority loads without one, the authority runs in-process only and never
// unloads, and the access policy reaches it whole, on every change.

#include "fake_module_host.h"
#include "logos_core.h"
#include "token_authority.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <thread>

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
        logos_core_set_access_policy(nullptr);
        // clear() re-attaches the stand-in for the next case.
        FakeHostFixture::TearDown();
    }
};

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
    // Once before the module holds its token, and again as its load commits.
    ASSERT_EQ(documents.size(), 2u);
    const json early = json::parse(documents[0]);
    EXPECT_EQ(early.value("not_loaded", json()), json::array({"someone"})) << early.dump();
    const json loaded = json::parse(documents[1]);
    EXPECT_EQ(loaded.value("not_loaded", json()), json::array({"someone"})) << loaded.dump();
    EXPECT_TRUE(loaded.contains("healthy")) << loaded.dump();

    ASSERT_EQ(logos_core_unload_module("healthy", false), 1);
    documents = stand_in::restrictionDocuments();
    ASSERT_EQ(documents.size(), 3u);
    const json unloaded = json::parse(documents[2]);
    EXPECT_FALSE(unloaded.contains("healthy")) << unloaded.dump();
    EXPECT_TRUE(unloaded.contains("not_loaded")) << unloaded.dump();
}

// Detector: a rule with no callers was dropped from the document, and a target
// absent from it is open to everyone.
TEST_F(TokenAuthorityTest, ARuleWithNoCallersIsSentNotDropped)
{
    ASSERT_EQ(logos_core_set_access_policy(
        R"({"version":1,"mode":"enforce","restrictions":{"quiet":{},"hushed":{"allowedCallers":[]}}})"), 0);
    plantModule("healthy", "report-ok");

    ASSERT_EQ(logos_core_load_module("healthy", LOGOS_LOAD_MODULE_ONLY), 1);
    const auto documents = stand_in::restrictionDocuments();
    ASSERT_FALSE(documents.empty());
    const json document = json::parse(documents.back());
    EXPECT_EQ(document.value("quiet", json()), json::array()) << document.dump();
    EXPECT_EQ(document.value("hushed", json()), json::array()) << document.dump();
}

// Detector: a module that died left its routes in capability_module's rules
// until something else loaded or unloaded.
TEST_F(TokenAuthorityTest, ACrashSendsThePolicyAgain)
{
    ASSERT_EQ(logos_core_set_access_policy(R"({"version":1,"mode":"enforce","restrictions":{}})"), 0);
    plantModule("doomed", "report-ok");
    ASSERT_EQ(logos_core_load_module("doomed", LOGOS_LOAD_MODULE_ONLY), 1);
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
