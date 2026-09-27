// A single-process runtime (no other process to run anything in): peering's rows
// join it, and it runs each import's facade itself, where elsewhere
// logos_host_remote does.
#include "fake_module_host.h"
#include "inproc_module_loader.h"
#include "module_manager.h"
#include "module_registry.h"

#include "logos_protocol.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <string>

using LogosCore::PlacementPolicy;
using LogosCore::decidePlacement;
using json = nlohmann::json;

namespace {

const json kEligible = {{"name", "x"}, {"inproc_eligible", true}};
const json kFacade = {{"name", "wallet"}, {"version", "0.1.0"}, {"concurrency", "multi"}};

PlacementPolicy policy(const std::string& text)
{
    PlacementPolicy parsed;
    std::string error;
    EXPECT_TRUE(LogosCore::parsePlacementPolicy(text, parsed, error)) << error;
    return parsed;
}

// The nix check links libpeering: there, a build without it is a failure, not a skip.
bool facadesBuilt()
{
    if (LogosCore::facadesRunInProcess()) return true;
    if (std::getenv("LOGOS_REQUIRE_TEST_FIXTURES")) ADD_FAILURE() << "built without libpeering";
    return false;
}

LogosCore::ModuleDescriptor facadeDescriptor()
{
    LogosCore::ModuleDescriptor desc;
    desc.name = "wallet";
    desc.format = LogosCore::kFacadeFormat;
    desc.rawMetadata = kFacade;
    return desc;
}

class InprocFacadeTest : public FakeHostFixture {};

// A peering_module that knows no imports.
char* noImports(const char* method, const char*, void*)
{
    const std::string name = method ? method : "";
    return lp_string_copy(name == "importDescriptor" ? R"({"error":"NOT_IMPORTED"})"
                                                     : R"({"error":"UNEXPECTED"})");
}

char* noMethods(void*) { return lp_string_copy("[]"); }

} // namespace

TEST(FacadePlacement, PeeringJoinsASingleProcessRuntimeAndStaysApartOtherwise)
{
    const PlacementPolicy single = policy(R"({"single_process":true})");
    const PlacementPolicy moved = policy(
        R"({"default":"inproc","modules":{"peering_module":"inproc","peering_identity":"inproc"}})");
    for (const char* name : {"peering_module", "peering_identity"}) {
        EXPECT_FALSE(decidePlacement(name, kEligible, "native-cdylib", true, {}).inProcess) << name;
        EXPECT_FALSE(decidePlacement(name, kEligible, "native-cdylib", true, moved).inProcess)
            << name << ": no policy moves it in";
        EXPECT_TRUE(decidePlacement(name, kEligible, "native-cdylib", true, single).inProcess) << name;
        EXPECT_TRUE(decidePlacement(name, kEligible, "native-cdylib", false, single).refused)
            << name << ": only the bundled one";
    }
    // The package modules still cannot run there.
    EXPECT_TRUE(decidePlacement("package_manager", kEligible, "native-cdylib", true, single).refused);
}

TEST(FacadePlacement, AFacadeRunsHereOnlyWhenNothingRunsApart)
{
    const auto apart = decidePlacement("wallet", kFacade, LogosCore::kFacadeFormat, false,
                                       policy(R"({"default":"inproc","modules":{"wallet":"inproc"}})"));
    EXPECT_FALSE(apart.inProcess) << "it faces the network: its own process when there are others";
    EXPECT_FALSE(apart.refused);
    const auto single = decidePlacement("wallet", kFacade, LogosCore::kFacadeFormat, false,
                                        policy(R"({"single_process":true})"));
    EXPECT_EQ(single.inProcess, LogosCore::facadesRunInProcess()) << single.reason;
    EXPECT_EQ(single.refused, !LogosCore::facadesRunInProcess());
    facadesBuilt();
}

TEST_F(InprocFacadeTest, OnlyTheFacadeLoaderTakesOneAndOnlyUnderSingleProcess)
{
    if (!facadesBuilt()) GTEST_SKIP() << "built without libpeering";
    LogosCore::InprocModuleLoader modules;
    const auto desc = facadeDescriptor();
    EXPECT_FALSE(modules.canHandle(desc));
    auto apart = ModuleManager::loaders().select(desc);
    ASSERT_TRUE(apart);
    EXPECT_NE(apart->id(), "inproc-facade");
    ASSERT_EQ(logos_core_set_placement_policy(R"({"single_process":true})"), 0);
    EXPECT_FALSE(modules.canHandle(desc)) << "it only refuses a facade it cannot run";
    auto here = ModuleManager::loaders().select(desc);
    ASSERT_TRUE(here);
    EXPECT_EQ(here->id(), "inproc-facade");
}

// libpeering's Facade starts in this process and asks peering_module about its
// import; this one knows none, so the load fails with that, and nothing is spawned.
TEST_F(InprocFacadeTest, ASingleProcessRuntimeRunsTheFacadeItself)
{
    if (!facadesBuilt()) GTEST_SKIP() << "built without libpeering";
    ASSERT_EQ(logos_core_set_placement_policy(R"({"single_process":true})"), 0);
    ASSERT_EQ(lp_token_isolate_identity("wallet"), LP_OK);
    ASSERT_EQ(lp_token_save_for("wallet", "peering_module", "stub-token"), LP_OK);
    lp_provider* peering = lp_provider_create("peering_module", nullptr);
    ASSERT_NE(peering, nullptr);
    ASSERT_EQ(lp_provider_save_token(peering, "wallet", "stub-token"), LP_OK);
    ASSERT_EQ(lp_provider_register(peering, noImports, noMethods, nullptr, nullptr), LP_OK);

    ASSERT_TRUE(ModuleManager::registry().registerFacade("wallet", kFacade, false));
    EXPECT_EQ(logos_core_load_module("wallet", LOGOS_LOAD_MODULE_ONLY), 0);
    lp_provider_destroy(peering);
    EXPECT_FALSE(logos_core_is_module_loaded("wallet"));
    const std::string reason = reasonFor("wallet", logos::module_state::kError);
    EXPECT_NE(reason.find("has no import named wallet"), std::string::npos) << reason;
    EXPECT_TRUE(ModuleManager::loaders().getAllPids().empty());
    ModuleManager::registry().forgetFacade("wallet");
}
