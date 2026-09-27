// A module's configuration: protected input, delivered by its loader with its
// credential on every start, and a load that fails when it cannot be delivered.
#include "fake_module_host.h"
#include "logos_core.h"
#include "module_manager.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

class ModuleConfigSetterTest : public ::testing::Test {
protected:
    void SetUp() override { logos_core_terminate_all(); logos_core_clear(); }
    void TearDown() override { logos_core_terminate_all(); logos_core_clear(); }
};

TEST_F(ModuleConfigSetterTest, EachModuleGetsOneWholeDocument)
{
    ASSERT_EQ(logos_core_set_module_config(R"({"a":{"endpoint":"x","n":[1,2]},"b":"text"})"), 0);
    EXPECT_EQ(ModuleManager::moduleConfiguration("a"), std::string(R"({"endpoint":"x","n":[1,2]})"));
    EXPECT_EQ(ModuleManager::moduleConfiguration("b"), std::string(R"("text")"));
    // Replaced whole, never merged; null removes; others are kept.
    ASSERT_EQ(logos_core_set_module_config(R"({"a":{"n":[3]},"b":null})"), 0);
    EXPECT_EQ(ModuleManager::moduleConfiguration("a"), std::string(R"({"n":[3]})"));
    EXPECT_FALSE(ModuleManager::moduleConfiguration("b").has_value());
    ASSERT_EQ(logos_core_set_module_config(nullptr), 0);
    EXPECT_FALSE(ModuleManager::moduleConfiguration("a").has_value());
}

TEST_F(ModuleConfigSetterTest, AMalformedDocumentChangesNothing)
{
    ASSERT_EQ(logos_core_set_module_config(R"({"a":{"kept":true}})"), 0);
    for (const char* bad : {"[]", "not json", R"({"a":{},"../x":{}})", R"({"core_service":{}})",
                            R"({"core":{}})"})
        EXPECT_EQ(logos_core_set_module_config(bad), -1) << bad;
    EXPECT_EQ(ModuleManager::moduleConfiguration("a"), std::string(R"({"kept":true})"));
}

TEST_F(ModuleConfigSetterTest, ProtectedInputBeforeStartOnly)
{
    ModuleManager::markStarted();
    EXPECT_EQ(logos_core_set_module_config(R"({"a":{}})"), -1);
    logos_core_clear();
    EXPECT_EQ(logos_core_set_module_config(R"({"a":{}})"), 0);
}

// The stand-in host, told how a host new enough to read a configuration behaves.
class ModuleConfigDeliveryTest : public FakeHostFixture {
protected:
    void SetUp() override
    {
        FakeHostFixture::SetUp();
        useFakeHost();
    }

    void TearDown() override
    {
        logos_test::unsetEnv("LOGOS_TEST_CONFIGURATION_LOG");
        FakeHostFixture::TearDown();
    }

    std::string useCurrentHost()
    {
        const std::string log = (tmp.path / "configurations").string();
        logos_test::setEnv("LOGOS_TEST_CONFIGURATION_LOG", log);
        return log;
    }

    static std::vector<std::string> lines(const std::string& file)
    {
        std::ifstream in(file);
        std::vector<std::string> out;
        for (std::string line; std::getline(in, line);) out.push_back(line);
        return out;
    }
};

// Detector: a module's configuration never reached it, however often it started.
TEST_F(ModuleConfigDeliveryTest, AModuleGetsItsDocumentOnEveryStart)
{
    const std::string log = useCurrentHost();
    ASSERT_EQ(logos_core_set_module_config(R"({"configured":{"endpoint":"x"}})"), 0);
    plantModule("configured", "report-ok");
    plantModule("unconfigured", "report-ok");

    ASSERT_EQ(logos_core_load_module("configured", LOGOS_LOAD_MODULE_ONLY), 1) << hostEventLog();
    ASSERT_EQ(logos_core_load_module("unconfigured", LOGOS_LOAD_MODULE_ONLY), 1) << hostEventLog();
    EXPECT_EQ(lines(log), (std::vector<std::string>{R"(configured {"endpoint":"x"})"}));

    ASSERT_EQ(logos_core_unload_module("configured", false), 1);
    ASSERT_EQ(logos_core_load_module("configured", LOGOS_LOAD_MODULE_ONLY), 1) << hostEventLog();
    EXPECT_EQ(spawnCount("configured"), 2);
    EXPECT_EQ(lines(log), (std::vector<std::string>{R"(configured {"endpoint":"x"})",
                                                    R"(configured {"endpoint":"x"})"}));
}

// A host that predates the flag refuses it and exits: the load fails instead of
// the module starting unconfigured.
TEST_F(ModuleConfigDeliveryTest, AnOlderHostFailsAConfiguredLoad)
{
    ASSERT_EQ(logos_core_set_module_config(R"({"configured":{"endpoint":"x"}})"), 0);
    plantModule("configured", "report-ok");

    EXPECT_EQ(logos_core_load_module("configured", LOGOS_LOAD_MODULE_ONLY), 0);
    EXPECT_FALSE(logos_core_is_module_loaded("configured"));
    EXPECT_TRUE(sawTransitionTo("configured", logos::module_state::kError));
}

// The real Qt plugin host takes no configuration in this version.
class QtHostModuleConfigTest : public ModuleConfigDeliveryTest {
protected:
    void SetUp() override
    {
        FakeHostFixture::SetUp();
        const char* host = std::getenv("TEST_REAL_HOST");
        if (!host || !fs::exists(host)) {
            if (std::getenv("LOGOS_REQUIRE_TEST_FIXTURES"))
                FAIL() << "TEST_REAL_HOST does not name a built module host";
            GTEST_SKIP() << "TEST_REAL_HOST not set";
        }
        logos_test::setEnv("LOGOS_HOST_PATH", host);
    }
};

TEST_F(QtHostModuleConfigTest, AQtPluginCannotBeGivenAConfiguration)
{
    ASSERT_EQ(logos_core_set_module_config(R"({"qt_configured":{"endpoint":"x"}})"), 0);
    plantModule("qt_configured", "a Qt plugin would be here");

    EXPECT_EQ(logos_core_load_module("qt_configured", LOGOS_LOAD_MODULE_ONLY), 0);
    const std::string reason = reasonFor("qt_configured", logos::module_state::kError);
    EXPECT_NE(reason.find("configuration"), std::string::npos) << reason;
}

} // namespace
