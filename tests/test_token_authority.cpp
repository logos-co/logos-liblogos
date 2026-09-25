// The runtime and its token authority, capability_module: nothing but the
// authority loads without one, the authority runs in-process only and never
// unloads, and the access policy reaches it whole, on every change.

#include "fake_module_host.h"
#include "logos_core.h"
#include "token_authority.h"

#include <nlohmann/json.hpp>

#include <string>

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
    ASSERT_EQ(documents.size(), 1u);
    const json loaded = json::parse(documents[0]);
    EXPECT_EQ(loaded.value("not_loaded", json()), json::array({"someone"})) << loaded.dump();
    EXPECT_TRUE(loaded.contains("healthy")) << loaded.dump();

    ASSERT_EQ(logos_core_unload_module("healthy", false), 1);
    documents = stand_in::restrictionDocuments();
    ASSERT_EQ(documents.size(), 2u);
    const json unloaded = json::parse(documents[1]);
    EXPECT_FALSE(unloaded.contains("healthy")) << unloaded.dump();
    EXPECT_TRUE(unloaded.contains("not_loaded")) << unloaded.dump();
}
