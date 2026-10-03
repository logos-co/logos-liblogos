// Peering in the engine: the bundled rows, facade records for imports, the
// export listener, the peering configuration as protected input, and what
// core_service lets a remote consumer do.
#include <gtest/gtest.h>
#include "logos_core.h"
#include "logos_protocol.h"
#include "core_service/embedded_core_service.h"
#include "token_authority.h"
#include "bootstrap_policy.h"
#include "module_manager.h"
#include "module_registry.h"
#include "peering_link.h"
#include "qt_test_adapter.h"
#include "test_platform.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct TmpDir {
    fs::path path = logos_test::makeTempDir("logos_peering_");
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void install(const fs::path& dir, const std::string& name)
{
    const fs::path moduleDir = dir / name;
    fs::create_directories(moduleDir);
    std::ofstream(moduleDir / "manifest.json")
        << json{{"name", name}, {"version", "1.0.0"}, {"type", "core"},
                {"main", name + "_plugin.so"}}.dump();
    std::ofstream(moduleDir / (name + "_plugin.so")) << "fake";
    std::ofstream(moduleDir / (name + "_plugin.metadata.json"))
        << json{{"name", name}, {"version", "1.0.0"}, {"transport", "qt_remote_plain"}}.dump();
}

json facade(const std::string& name) { return {{"name", name}, {"version", "0.1.0"}}; }

class FacadeRecordsTest : public ::testing::Test {
protected:
    TmpDir modules;
    void SetUp() override { logos_core_terminate_all(); logos_core_clear(); }
    void TearDown() override { logos_core_terminate_all(); logos_core_clear(); }
    ModuleRegistry& registry() { return ModuleManager::registry(); }
};

} // namespace

TEST(PeeringRows, BothModulesAreReservedAndHosted)
{
    using namespace logos::bootstrap;
    for (const char* name : {"peering_module", "Peering_Identity", "peering_control"})
        EXPECT_TRUE(isReservedName(name)) << name;
    // peering_module gates its callers itself; the root key's holder does not.
    EXPECT_TRUE(isExemptTarget("peering_module"));
    EXPECT_FALSE(isExemptTarget("peering_identity"));
    EXPECT_EQ(defaultPlacement("peering_module"), Placement::Subprocess);
    EXPECT_EQ(defaultPlacement("peering_identity"), Placement::Subprocess);
    EXPECT_TRUE(rowFor("peering_module")->pinned);
    // Facades (user modules) go first, then peering_module, then the key.
    EXPECT_LT(teardownRank("some_import"), teardownRank("peering_module"));
    EXPECT_LT(teardownRank("peering_module"), teardownRank("peering_identity"));
    EXPECT_LT(teardownRank("peering_identity"), teardownRank("capability_module"));
    EXPECT_TRUE(hostServicesFor("peering_module", true).empty());
}

TEST_F(FacadeRecordsTest, AFacadeIsKnownWithoutAFileAndSurvivesRefresh)
{
    ASSERT_TRUE(registry().registerFacade("monerod_module", facade("monerod_module"), false));
    EXPECT_TRUE(registry().isKnown("monerod_module"));
    EXPECT_TRUE(registry().isFacade("monerod_module"));
    EXPECT_EQ(registry().moduleFormat("monerod_module"), "peer-facade");
    EXPECT_EQ(registry().moduleMetadata("monerod_module").value("version", ""), "0.1.0");
    logos_core_add_modules_dir(modules.path.string().c_str());
    logos_core_refresh_modules();
    EXPECT_TRUE(registry().isFacade("monerod_module"));
    EXPECT_TRUE(registry().forgetFacade("monerod_module"));
    EXPECT_FALSE(registry().isKnown("monerod_module"));
}

TEST_F(FacadeRecordsTest, ALocalCopyWaitsWhileTheImportExists)
{
    install(modules.path, "monerod_module");
    logos_core_add_modules_dir(modules.path.string().c_str());
    logos_core_refresh_modules();
    ASSERT_TRUE(registry().isKnown("monerod_module"));
    // prefer local: the installed module keeps the name.
    EXPECT_FALSE(registry().registerFacade("monerod_module", facade("monerod_module"), false));
    EXPECT_FALSE(registry().isFacade("monerod_module"));
    // prefer remote: the import takes it, and discovery leaves it be.
    ASSERT_TRUE(registry().registerFacade("monerod_module", facade("monerod_module"), true));
    logos_core_refresh_modules();
    EXPECT_TRUE(registry().isFacade("monerod_module"));
    // Once the import goes, the local module is found again.
    ASSERT_TRUE(registry().forgetFacade("monerod_module"));
    logos_core_refresh_modules();
    EXPECT_TRUE(registry().isKnown("monerod_module"));
    EXPECT_FALSE(registry().isFacade("monerod_module"));
    EXPECT_EQ(registry().moduleFormat("monerod_module"), "native-cdylib");
}

TEST(PeeringLink, AnExportListensBesideTheModulesOwnTransports)
{
    const json added = json::parse(logos::peering_link::withExportListener("", "127.0.0.1"));
    ASSERT_EQ(added.size(), 2u);
    EXPECT_EQ(added[0], json({{"protocol", "qt_remote_plain"}}));
    EXPECT_EQ(added[1], json({{"protocol", "tls_tcp"}, {"host", "127.0.0.1"}, {"port", 0}}));
    const json kept = json::parse(logos::peering_link::withExportListener(
        R"([{"protocol":"inproc"},{"protocol":"tcp","host":"0.0.0.0","port":9000}])", "0.0.0.0"));
    ASSERT_EQ(kept.size(), 3u);
    EXPECT_EQ(kept[1].value("protocol", ""), "tcp");
    EXPECT_EQ(kept[2].value("protocol", ""), "tls_tcp");
}

TEST(PeeringLink, TheConfigurationIsAProtectedObject)
{
    logos_core_terminate_all();
    logos_core_clear();
    EXPECT_EQ(logos_core_set_peering_config("[1,2]"), -1);
    EXPECT_EQ(logos_core_set_peering_config(R"({"control":{"enabled":true}})"), 0);
    EXPECT_TRUE(logos::peering_link::configured());
    EXPECT_EQ(logos_core_set_peering_config(""), 0);
    EXPECT_FALSE(logos::peering_link::configured());
    ASSERT_EQ(logos_core_set_peering_config(R"({"control":{"enabled":true}})"), 0);
    // Without peering_module installed, start carries on without peering.
    logos_core_start();
    EXPECT_EQ(logos_core_set_peering_config("{}"), -1);
    EXPECT_FALSE(ModuleManager::registry().isLoaded("peering_module"));
    logos_core_terminate_all();
    logos_core_clear();
    EXPECT_FALSE(logos::peering_link::configured());
}

// Runtime Control (logos-lips runtime §9): each operation needs the remote policy's grant.
TEST(RuntimeControl, ARemoteConsumerGetsOnlyTheMethodsItsRuntimeIsGranted)
{
    logos_core_terminate_all();
    logos_core_clear();
    logos_core_start();
    const auto call = [](const char* method, const json& args = json::array()) {
        char* out = logos::core_service::dispatchAs(R"({"kind":"remote","peer":"peer-1","name":"ctl"})",
                                                    method, args.dump().c_str());
        const json value = out ? json::parse(out, nullptr, false) : json();
        lp_string_free(out);
        return value;
    };
    const auto refused = [](const json& reply) {
        return reply.is_object() && reply.value("code", std::string()) == "NOT_AUTHORISED";
    };
    const auto policy = [](const json& document) { return logos::authority::setRemotePolicy(document.dump()); };

    EXPECT_TRUE(refused(call("getStatus")));
    ASSERT_TRUE(policy({{"peer-1/ctl", {{"core_service", {"getStatus", "admitConsumer", "evaluateRemoteAccess"}}}}}));
    EXPECT_FALSE(refused(call("getStatus"))) << call("getStatus").dump();
    EXPECT_TRUE(refused(call("listModules", {"all"})));
    // Never the shell's or peering's, granted or not.
    EXPECT_TRUE(refused(call("admitConsumer", {"viewer", "presentation"})));
    EXPECT_TRUE(refused(call("evaluateRemoteAccess", {"peer-1", "ctl", "x"})));
    // "*" is every export, never core_service; another consumer has no grant.
    ASSERT_TRUE(policy({{"peer-1/*", {"*"}}}));
    EXPECT_TRUE(refused(call("getStatus")));
    ASSERT_TRUE(policy({{"peer-1/ctl", {{"core_service", "*"}}}}));
    EXPECT_FALSE(refused(call("getStatus")));
    char* other = logos::core_service::dispatchAs(R"({"kind":"remote","peer":"peer-1","name":"other"})",
                                                  "getStatus", "[]");
    EXPECT_TRUE(refused(json::parse(other ? other : "null", nullptr, false)));
    lp_string_free(other);

    // Forwarding needs a grant on the target method too; the exact target wins over "*".
    ASSERT_TRUE(policy({{"peer-1/ctl", {{"core_service", {"callModuleMethod", "watchModuleEvents"}},
                                        {"wallet", {"balance"}}, {"*", "*"}}}}));
    EXPECT_TRUE(refused(call("callModuleMethod", {"wallet", "send", json::array()})));
    const json unloaded = call("callModuleMethod", {"wallet", "balance", json::array()});
    EXPECT_EQ(unloaded.value("code", std::string()), "MODULE_NOT_LOADED") << unloaded.dump();
    EXPECT_EQ(call("watchModuleEvents", {"ledger", ""}), json(false)) << "granted, not loaded";
    // Never the runtime's own modules, whatever "*" grants.
    for (const char* own : {"capability_module", "modules_state", "peering_module", "package_manager"}) {
        const json closed = call("callModuleMethod", {own, "anything", json::array()});
        EXPECT_EQ(closed.value("error", json::object()).value("code", std::string()), "unauthorized")
            << own << ": " << closed.dump();
        EXPECT_EQ(call("watchModuleEvents", {own, ""}), json(false)) << own;
    }

    ASSERT_TRUE(policy(json::object()));
    logos_core_terminate_all();
    logos_core_clear();
}

#ifndef _WIN32
namespace {

// Destroyed after the statics the runtime made while running: time for a thread
// still running during exit to reach one of them.
struct SlowTeardown {
    bool armed = false;
    ~SlowTeardown()
    {
        if (armed) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
} g_slowTeardown;

// tests/fixtures/peering_stub_module.cpp, bundled as peering_module in `dir`.
bool installStub(const fs::path& dir)
{
    const fs::path stub = logos_test::thisExecutable().parent_path().parent_path() / "lib"
        / "peering_stub_module.fixture";
#ifdef __APPLE__
    const std::string file = "peering_module_plugin.dylib";
#else
    const std::string file = "peering_module_plugin.so";
#endif
    const fs::path moduleDir = dir / "peering_module";
    std::error_code ec;
    fs::create_directories(moduleDir, ec);
    if (!fs::copy_file(stub, moduleDir / file, fs::copy_options::overwrite_existing, ec)) return false;
    json main = json::object();
    for (const char* os : {"darwin", "linux"})
        for (const char* arch : {"arm64", "aarch64", "amd64", "x86_64"})
            for (const char* suffix : {"", "-dev"})
                main[std::string(os) + "-" + arch + suffix] = file;
    std::ofstream(moduleDir / "manifest.json")
        << json{{"name", "peering_module"}, {"version", "0.1.0"}, {"type", "core"}, {"main", main}}.dump();
    std::ofstream(moduleDir / "peering_module_plugin.metadata.json")
        << json{{"name", "peering_module"}, {"version", "0.1.0"}, {"type", "core"},
                {"interface", "universal"}, {"transport", "qt_remote_plain"},
                {"main", "peering_module_plugin"}, {"dependencies", json::array()},
                {"logos_protocol_version", LOGOS_PROTOCOL_VERSION_STRING},
                {"inproc_eligible", true}}.dump();
    return true;
}

} // namespace

// exit() with peering live, as an app's error path does with its runtime running:
// the child below starts peering_link, whose first job is still running, and exits.
TEST(PeeringLink, AnAppMayExitWithPeeringLive)
{
    const char* bundled = std::getenv("TEST_BUNDLED_MODULES_DIR");
    if (!bundled || !*bundled) {
        if (std::getenv("LOGOS_REQUIRE_TEST_FIXTURES")) FAIL() << "TEST_BUNDLED_MODULES_DIR not set";
        GTEST_SKIP() << "TEST_BUNDLED_MODULES_DIR not set";
    }
    TmpDir stub;
    ASSERT_TRUE(installStub(stub.path));
    logos_test::setEnv("PEERING_EXIT_BUNDLED", bundled);
    logos_test::setEnv("PEERING_EXIT_STUB", stub.path.string());
    // The child overwrites freed memory, so a use after free there faults every time.
    logos_test::setEnv("MallocScribble", "1");
    logos_test::setEnv("MALLOC_PERTURB_", "85");
    logos_test::Child child;
    const bool started = child.start(logos_test::thisExecutable().string(),
        {"--gtest_filter=PeeringLinkChild.DISABLED_ExitsWithPeeringLive",
         "--gtest_also_run_disabled_tests"}, true);
    for (const char* name : {"PEERING_EXIT_BUNDLED", "PEERING_EXIT_STUB", "MallocScribble", "MALLOC_PERTURB_"})
        logos_test::unsetEnv(name);
    ASSERT_TRUE(started);
    std::string output;
    char buffer[512];
    while (output.find("LIVE\n") == std::string::npos) {
        const size_t n = child.read(buffer, sizeof buffer);
        if (n == 0) break;
        output.append(buffer, n);
    }
    const int code = child.wait(std::chrono::seconds(30));
    EXPECT_NE(output.find("CONFIGURED"), std::string::npos) << "peering never started\n" << output;
    EXPECT_EQ(code, 3) << "a negative code is the signal that ended it\n" << output;
}

// The child of AnAppMayExitWithPeeringLive, never run on its own.
TEST(PeeringLinkChild, DISABLED_ExitsWithPeeringLive)
{
    const char* bundled = std::getenv("PEERING_EXIT_BUNDLED");
    const char* stub = std::getenv("PEERING_EXIT_STUB");
    ASSERT_TRUE(bundled && stub);
    logos::authority::detach();
    const char* dirs[] = {bundled, stub, nullptr};
    ASSERT_EQ(logos_core_set_bundled_modules_dirs(dirs), 0);
    ASSERT_EQ(logos_core_set_placement_policy(R"({"single_process":true})"), 0);
    ASSERT_EQ(logos_core_set_peering_config(R"({"name":"exit-test"})"), 0);
    logos_core_start();
    std::printf("LIVE\n");
    std::fflush(stdout);
    g_slowTeardown.armed = true;
    std::exit(3);
}
#endif
