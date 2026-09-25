// The bootstrap policy: privilege comes from the table and from being bundled,
// never from a name alone.
#include <gtest/gtest.h>
#include "logos_core.h"
#include "bootstrap_policy.h"
#include "module_manager.h"
#include "module_registry.h"
#include "qt_test_adapter.h"
#include "test_platform.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace logos::bootstrap;

namespace {

struct TmpDir {
    fs::path path = logos_test::makeTempDir("logos_bootstrap_");
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
};

// A plain module as a package directory: manifest, stand-in binary and sidecar.
void install(const fs::path& dir, const std::string& name)
{
    const fs::path moduleDir = dir / name;
    fs::create_directories(moduleDir);
    nlohmann::json manifest{{"name", name}, {"version", "1.0.0"}, {"type", "core"},
                            {"main", name + "_plugin.so"}};
    std::ofstream(moduleDir / "manifest.json") << manifest.dump();
    std::ofstream(moduleDir / (name + "_plugin.so")) << "fake";
    nlohmann::json sidecar{{"name", name}, {"version", "1.0.0"},
                           {"transport", "qt_remote_plain"}};
    std::ofstream(moduleDir / (name + "_plugin.metadata.json")) << sidecar.dump();
}

int setBundled(const std::vector<std::string>& dirs)
{
    std::vector<const char*> list;
    for (const std::string& dir : dirs) list.push_back(dir.c_str());
    list.push_back(nullptr);
    return logos_core_set_bundled_modules_dirs(list.data());
}

bool under(const std::string& path, const TmpDir& dir)
{
    return fs::weakly_canonical(path).string().rfind(fs::weakly_canonical(dir.path).string(), 0) == 0;
}

class BundledDirsTest : public ::testing::Test {
protected:
    TmpDir bundled;
    TmpDir user;
    void SetUp() override { logos_core_terminate_all(); logos_core_clear(); }
    void TearDown() override { logos_core_terminate_all(); logos_core_clear(); }
    ModuleRegistry& registry() { return ModuleManager::registry(); }
};

} // namespace

TEST(BootstrapPolicy, ReservedNamesIgnoreCase)
{
    for (const char* name : {"capability_module", "Capability_Module", "CORE_SERVICE", "Core",
                             "modules_state", "package_manager", "package_downloader",
                             "logos_anything", "Logos_X", "basecamp", "logoscore",
                             "standalone", "module_viewer"})
        EXPECT_TRUE(isReservedName(name)) << name;
    for (const char* name : {"my_module", "chat", "capability", "logos"})
        EXPECT_FALSE(isReservedName(name)) << name;
}

// The table replaced kTrustedCallers, kExemptTargets and the name-based grant.
TEST(BootstrapPolicy, TheTableKeepsTheOldTrustAndGrants)
{
    EXPECT_EQ(trustedCallers(), (std::vector<std::string>{"core", "core_service"}));
    for (const char* name : {"capability_module", "core", "core_service"})
        EXPECT_TRUE(isExemptTarget(name)) << name;
    EXPECT_FALSE(isExemptTarget("package_manager"));
    EXPECT_EQ(hostServicesFor("capability_module", true),
              (std::vector<std::string>{"token_registry", "token_delivery"}));
    EXPECT_TRUE(hostServicesFor("capability_module", false).empty());
    EXPECT_TRUE(hostServicesFor("my_module", true).empty());
    EXPECT_LT(teardownRank("my_module"), teardownRank("modules_state"));
    EXPECT_LT(teardownRank("modules_state"), teardownRank("capability_module"));
}

TEST_F(BundledDirsTest, AReservedNameResolvesOnlyFromABundledDirectory)
{
    install(bundled.path, "capability_module");
    install(user.path, "capability_module");
    install(user.path, "modules_state");
    install(user.path, "Package_Manager");
    install(user.path, "chat_module");
    ASSERT_EQ(setBundled({bundled.str()}), 0);
    logos_core_add_modules_dir(user.str().c_str());
    logos_core_refresh_modules();

    ASSERT_TRUE(registry().isKnown("capability_module"));
    EXPECT_TRUE(under(registry().modulePath("capability_module"), bundled));
    EXPECT_TRUE(registry().isBundled("capability_module"));
    EXPECT_FALSE(registry().isKnown("modules_state")) << "a reserved name from a user directory";
    EXPECT_FALSE(registry().isKnown("Package_Manager")) << "reserved names ignore case";
    ASSERT_TRUE(registry().isKnown("chat_module"));
    EXPECT_FALSE(registry().isBundled("chat_module"));
}

// A bundle may be a symlink farm (a nix buildEnv) whose links point elsewhere.
TEST_F(BundledDirsTest, ASymlinkFarmIsStillBundled)
{
    TmpDir store;
    install(store.path, "capability_module");
    install(store.path, "modules_state");
    std::error_code ec;
    fs::create_directories(bundled.path / "capability_module");
    for (const auto& entry : fs::directory_iterator(store.path / "capability_module"))
        fs::create_symlink(entry.path(), bundled.path / "capability_module" / entry.path().filename(),
                           ec);
    fs::create_directory_symlink(store.path / "modules_state", bundled.path / "modules_state", ec);
    if (ec) GTEST_SKIP() << "no symlinks here: " << ec.message();
    ASSERT_EQ(setBundled({bundled.str()}), 0);
    logos_core_refresh_modules();
    for (const char* name : {"capability_module", "modules_state"}) {
        ASSERT_TRUE(registry().isKnown(name)) << name;
        EXPECT_TRUE(registry().isBundled(name)) << name;
    }
}

// An ordinary module keeps the old rule: the last directory scanned wins.
TEST_F(BundledDirsTest, AnOrdinaryUserCopyStillWins)
{
    install(bundled.path, "chat_module");
    install(user.path, "chat_module");
    ASSERT_EQ(setBundled({bundled.str()}), 0);
    logos_core_add_modules_dir(user.str().c_str());
    logos_core_refresh_modules();
    EXPECT_TRUE(under(registry().modulePath("chat_module"), user));
    EXPECT_FALSE(registry().isBundled("chat_module"));
}

// An embedder that names no bundled directory sees no change.
TEST_F(BundledDirsTest, WithoutBundledDirectoriesNamesLoadAsBefore)
{
    install(user.path, "capability_module");
    logos_core_add_modules_dir(user.str().c_str());
    logos_core_refresh_modules();
    EXPECT_TRUE(registry().isKnown("capability_module"));
    EXPECT_FALSE(registry().isBundled("capability_module"));
}

TEST_F(BundledDirsTest, BundledDirectoriesAreProtectedOnceStarted)
{
    EXPECT_EQ(logos_core_set_bundled_modules_dirs(nullptr), -1);
    logos_core_start();
    EXPECT_EQ(setBundled({bundled.str()}), -1);
    logos_core_clear();
    EXPECT_EQ(setBundled({bundled.str()}), 0);
}
