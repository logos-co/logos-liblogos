// package_manager's settings: the embedder hands them over before start, and the
// runtime applies them as package_manager loads, failing closed on the policy.
#include <gtest/gtest.h>
#include "logos_core.h"
#include "package_config.h"
#include "qt_test_adapter.h"

#include <string>
#include <utility>
#include <vector>

using namespace logos::package_config;

namespace {

std::vector<std::pair<std::string, std::string>> methods(const std::vector<Call>& calls)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const Call& call : calls) out.emplace_back(call.method, call.arg);
    return out;
}

} // namespace

TEST(PackageConfig, EveryKeyBecomesItsSetterInOrder)
{
    std::vector<Call> calls;
    std::string error;
    ASSERT_TRUE(parse(R"({"embedded_modules_dirs":["/app/modules","/app/modules-pkg"],
                          "user_modules_dir":"/u/modules",
                          "embedded_ui_plugins_dirs":["/app/plugins"],
                          "user_ui_plugins_dir":"/u/plugins",
                          "keyring_dir":"/u/keyring","signature_policy":"require"})",
                      calls, error)) << error;
    EXPECT_EQ(methods(calls), (std::vector<std::pair<std::string, std::string>>{
        {"setEmbeddedModulesDirectory", "/app/modules"},
        {"addEmbeddedModulesDirectory", "/app/modules-pkg"},
        {"setUserModulesDirectory", "/u/modules"},
        {"setEmbeddedUiPluginsDirectory", "/app/plugins"},
        {"setUserUiPluginsDirectory", "/u/plugins"},
        {"setKeyringDirectory", "/u/keyring"},
        {"setSignaturePolicy", "require"},
    }));
    EXPECT_TRUE(calls.back().failClosed);
    EXPECT_FALSE(calls.front().failClosed);
}

TEST(PackageConfig, MalformedDocumentsAreRefused)
{
    std::vector<Call> calls;
    std::string error;
    for (const char* bad : {"[]", "not json", R"({"user_modules_dir":7})",
                            R"({"embedded_modules_dirs":"/one"})", R"({"signature_policy":"lax"})",
                            R"({"modules_dir":"/typo"})", R"({"user_modules_dir":""})"})
        EXPECT_FALSE(parse(bad, calls, error)) << bad;
}

TEST(PackageConfig, OnlyThePolicyFailsClosed)
{
    std::vector<Call> calls;
    std::string error;
    ASSERT_TRUE(parse(R"({"user_modules_dir":"/u","signature_policy":"require"})", calls, error));
    std::vector<std::string> tried;
    auto refuseDirectories = [&](const std::string& method, const std::string&) {
        tried.push_back(method);
        return method == "setSignaturePolicy";
    };
    EXPECT_TRUE(apply(calls, refuseDirectories, error)) << "a directory is best-effort";
    auto refusePolicy = [&](const std::string& method, const std::string&) {
        return method != "setSignaturePolicy";
    };
    EXPECT_FALSE(apply(calls, refusePolicy, error));
    EXPECT_NE(error.find("setSignaturePolicy"), std::string::npos) << error;
}

class PackageConfigSetterTest : public ::testing::Test {
protected:
    void SetUp() override { logos_core_terminate_all(); logos_core_clear(); }
    void TearDown() override { logos_core_terminate_all(); logos_core_clear(); }
};

TEST_F(PackageConfigSetterTest, ProtectedInputBeforeStartOnly)
{
    EXPECT_EQ(logos_core_set_package_config(R"({"signature_policy":"lax"})"), -1);
    EXPECT_TRUE(current().empty()) << "a refused document leaves nothing behind";
    ASSERT_EQ(logos_core_set_package_config(R"({"user_modules_dir":"/u"})"), 0);
    EXPECT_EQ(current().size(), 1u);
    logos_core_start();
    EXPECT_EQ(logos_core_set_package_config(R"({"user_modules_dir":"/v"})"), -1);
    logos_core_clear();
    EXPECT_TRUE(current().empty()) << "cleared with the rest of the runtime's configuration";
}
