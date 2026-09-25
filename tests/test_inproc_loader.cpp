// Modules the runtime places in-process: the decision, and the real bundled
// capability_module and modules_state running in this test's process.
#include <gtest/gtest.h>
#include "logos_core.h"
#include "inproc_module_loader.h"
#include "capability_authority.h"
#include "module_manager.h"
#include "module_registry.h"
#include "qt_test_adapter.h"
#include "logos_protocol.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>
#include <string>
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
    EXPECT_TRUE(decidePlacement("package_manager", kEligible, "native-cdylib", true, none).inProcess);
    const PlacementPolicy out =
        policy(R"({"modules":{"modules_state":"subprocess","capability_module":"subprocess"}})");
    EXPECT_FALSE(decidePlacement("modules_state", kEligible, "native-cdylib", true, out).inProcess);
    EXPECT_TRUE(decidePlacement("capability_module", kEligible, "native-cdylib", true, out).inProcess)
        << "no policy moves capability_module out";
    EXPECT_FALSE(decidePlacement("capability_module", kEligible, "native-cdylib", false, out).inProcess)
        << "but only a bundled one runs here";
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
        const char* dir = std::getenv("TEST_BUNDLED_MODULES_DIR");
        if (!dir || !*dir) {
            if (std::getenv("LOGOS_REQUIRE_TEST_FIXTURES")) FAIL() << "TEST_BUNDLED_MODULES_DIR not set";
            GTEST_SKIP() << "TEST_BUNDLED_MODULES_DIR not set";
        }
        bundled = dir;
    }

    void TearDown() override
    {
        logos_core_terminate_all();
        logos_core_clear();
    }
};

// One test, because an image that ran in this process stays mapped: loading it
// again, in any later test, needs a restart.
TEST_F(InprocBundledTest, TheRuntimesModulesRunInProcessUntilUnloaded)
{
    ASSERT_EQ(setBundled(bundled), 0);
    ASSERT_EQ(logos_core_set_placement_policy(R"({"default":"subprocess"})"), 0);
    logos_core_start();
    EXPECT_EQ(logos_core_set_placement_policy("{}"), -1) << "protected once started";

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

    EXPECT_EQ(logos_core_unload_module("modules_state", false), 1);
    EXPECT_FALSE(loaded("modules_state"));
    EXPECT_FALSE(logos::authority::resolveCaller(credential.c_str(), "inproc").has_value())
        << "unloading retires the admission";
    EXPECT_TRUE(call("modules_state", "list_modules").is_null()) << "its provider is withdrawn";
    EXPECT_NE(logos_core_load_module("modules_state", LOGOS_LOAD_MODULE_ONLY), 1)
        << "its image is still mapped";
}
