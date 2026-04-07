#include <gtest/gtest.h>
#include "logos_core.h"
#include "plugin_manager.h"
#include "plugin_registry.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <filesystem>

static void clearPluginState()
{
    PluginManager::terminateAll();
    PluginManager::registry().clear();
}

class PluginManagerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        clearPluginState();
    }

    void TearDown() override
    {
        clearPluginState();
    }
};

TEST_F(PluginManagerTest, GetLoadedPlugins_ReturnsEmptyList)
{
    EXPECT_TRUE(PluginManager::registry().loadedPluginNames().empty());
}

TEST_F(PluginManagerTest, GetKnownPlugins_ReturnsEmptyList)
{
    EXPECT_TRUE(PluginManager::registry().knownPluginNames().empty());
}

TEST_F(PluginManagerTest, GetKnownPlugins_ReturnsCorrectEntries)
{
    PluginManager::registry().registerPlugin("plugin1", "/path/to/plugin1.dylib");
    PluginManager::registry().registerPlugin("plugin2", "/path/to/plugin2.dylib");

    ASSERT_EQ(PluginManager::registry().knownPluginNames().size(), 2u);
    EXPECT_EQ(PluginManager::registry().pluginPath("plugin1"), "/path/to/plugin1.dylib");
    EXPECT_EQ(PluginManager::registry().pluginPath("plugin2"), "/path/to/plugin2.dylib");
}

TEST_F(PluginManagerTest, IsPluginLoaded_ReturnsFalseForUnloaded)
{
    EXPECT_FALSE(PluginManager::isPluginLoaded("nonexistent_plugin"));
}

TEST_F(PluginManagerTest, IsPluginKnown_ReturnsFalseForUnknown)
{
    EXPECT_FALSE(PluginManager::registry().isKnown("nonexistent_plugin"));
}

TEST_F(PluginManagerTest, IsPluginKnown_ReturnsTrueForKnown)
{
    PluginManager::registry().registerPlugin("test_plugin", "/path/to/plugin");

    EXPECT_TRUE(PluginManager::registry().isKnown("test_plugin"));
}

TEST_F(PluginManagerTest, GetLoadedPluginsCStr_ReturnsNullTerminatedArrayWhenEmpty)
{
    char** result = PluginManager::getLoadedPluginsCStr();

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);

    delete[] result;
}

TEST_F(PluginManagerTest, GetKnownPluginsCStr_ReturnsNullTerminatedArrayWhenEmpty)
{
    char** result = PluginManager::getKnownPluginsCStr();

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);

    delete[] result;
}

TEST_F(PluginManagerTest, GetKnownPluginsCStr_ReturnsCorrectArray)
{
    PluginManager::registry().registerPlugin("plugin1", "/path/to/plugin1");
    PluginManager::registry().registerPlugin("plugin2", "/path/to/plugin2");

    char** result = PluginManager::getKnownPluginsCStr();

    ASSERT_NE(result, nullptr);
    ASSERT_NE(result[0], nullptr);
    ASSERT_NE(result[1], nullptr);
    EXPECT_EQ(result[2], nullptr);

    std::set<std::string> plugins;
    plugins.insert(result[0]);
    plugins.insert(result[1]);

    EXPECT_TRUE(plugins.count("plugin1"));
    EXPECT_TRUE(plugins.count("plugin2"));

    delete[] result[0];
    delete[] result[1];
    delete[] result;
}

TEST_F(PluginManagerTest, LoadPlugin_ReturnsFalseForUnknownPlugin)
{
    bool result = PluginManager::loadPlugin("nonexistent_plugin");
    EXPECT_FALSE(result);
}

TEST_F(PluginManagerTest, UnloadPlugin_ReturnsFalseForNotLoaded)
{
    bool result = PluginManager::unloadPlugin("nonexistent_plugin");
    EXPECT_FALSE(result);
}

TEST_F(PluginManagerTest, ResolveDependencies_ReturnsEmptyForEmptyInput)
{
    std::vector<std::string> result = PluginManager::resolveDependencies({});
    EXPECT_TRUE(result.empty());
}

TEST_F(PluginManagerTest, ResolveDependencies_ReturnsEmptyForUnknownPlugin)
{
    std::vector<std::string> requested = {"unknown_plugin"};

    std::vector<std::string> result = PluginManager::resolveDependencies(requested);
    EXPECT_TRUE(result.empty());
}

TEST_F(PluginManagerTest, ResolveDependencies_ReturnsSinglePluginWithNoDeps)
{
    PluginManager::registry().registerPlugin("plugin_a", "/path/to/plugin_a");
    PluginManager::registry().registerDependencies("plugin_a", {});

    std::vector<std::string> requested = {"plugin_a"};

    std::vector<std::string> result = PluginManager::resolveDependencies(requested);

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], "plugin_a");
}

TEST_F(PluginManagerTest, ResolveDependencies_ReturnsCorrectOrder)
{
    PluginManager::registry().registerPlugin("plugin_a", "/path/to/plugin_a");
    PluginManager::registry().registerPlugin("plugin_b", "/path/to/plugin_b");
    PluginManager::registry().registerDependencies("plugin_a", {"plugin_b"});
    PluginManager::registry().registerDependencies("plugin_b", {});

    std::vector<std::string> requested = {"plugin_a"};

    std::vector<std::string> result = PluginManager::resolveDependencies(requested);

    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], "plugin_b");
    EXPECT_EQ(result[1], "plugin_a");
}

TEST_F(PluginManagerTest, ResolveDependencies_HandlesTransitiveDeps)
{
    PluginManager::registry().registerPlugin("plugin_a", "/path/to/plugin_a");
    PluginManager::registry().registerPlugin("plugin_b", "/path/to/plugin_b");
    PluginManager::registry().registerPlugin("plugin_c", "/path/to/plugin_c");
    PluginManager::registry().registerDependencies("plugin_a", {"plugin_b"});
    PluginManager::registry().registerDependencies("plugin_b", {"plugin_c"});
    PluginManager::registry().registerDependencies("plugin_c", {});

    std::vector<std::string> requested = {"plugin_a"};

    std::vector<std::string> result = PluginManager::resolveDependencies(requested);

    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], "plugin_c");
    EXPECT_EQ(result[1], "plugin_b");
    EXPECT_EQ(result[2], "plugin_a");
}

TEST_F(PluginManagerTest, LoadPluginWithDependencies_AbortsForNull)
{
    EXPECT_DEATH(logos_core_load_plugin_with_dependencies(nullptr), "");
}

TEST_F(PluginManagerTest, LoadPluginWithDependencies_ReturnsZeroForUnknown)
{
    int result = logos_core_load_plugin_with_dependencies("unknown_plugin");
    EXPECT_EQ(result, 0);
}

TEST_F(PluginManagerTest, SetPluginsDir_SetsFirstDirectory)
{
    PluginManager::setPluginsDir("/tmp/test_plugins");
    ASSERT_EQ(PluginManager::registry().pluginsDirs().size(), 1u);
    EXPECT_EQ(PluginManager::registry().pluginsDirs()[0], "/tmp/test_plugins");
}

TEST_F(PluginManagerTest, AddPluginsDir_AppendsDirectory)
{
    PluginManager::setPluginsDir("/tmp/dir1");
    PluginManager::addPluginsDir("/tmp/dir2");
    PluginManager::addPluginsDir("/tmp/dir3");

    ASSERT_EQ(PluginManager::registry().pluginsDirs().size(), 3u);
    EXPECT_EQ(PluginManager::registry().pluginsDirs()[0], "/tmp/dir1");
    EXPECT_EQ(PluginManager::registry().pluginsDirs()[1], "/tmp/dir2");
    EXPECT_EQ(PluginManager::registry().pluginsDirs()[2], "/tmp/dir3");
}

TEST_F(PluginManagerTest, GetPluginsDirs_ReturnsEmptyAfterClear)
{
    PluginManager::setPluginsDir("/tmp/dir1");
    clearPluginState();
    EXPECT_TRUE(PluginManager::registry().pluginsDirs().empty());
}

static std::string make_temp_dir()
{
    static int counter = 0;
    auto base = std::filesystem::temp_directory_path() / ("logos_pm_test_" + std::to_string(counter++));
    std::filesystem::create_directories(base);
    return base.string();
}

static void createFakeModule(const std::string& parentDir,
                             const std::string& moduleName,
                             const std::string& mainFile,
                             const std::string& type = "core")
{
    const auto modDir = std::filesystem::path(parentDir) / moduleName;
    std::filesystem::create_directories(modDir);

    nlohmann::json manifest;
    manifest["name"] = moduleName;
    manifest["version"] = "1.0.0";
    manifest["type"] = type;
    manifest["main"] = mainFile;
    manifest["description"] = "Fake test module";

    std::ofstream mf(modDir / "manifest.json");
    mf << manifest.dump();
    mf.close();

    std::ofstream bf(modDir / mainFile);
    bf << "fake";
    bf.close();
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_DoesNotCrashWithEmptyDir)
{
    std::string tmpDir = make_temp_dir();

    PluginManager::setPluginsDir(tmpDir.c_str());
    PluginManager::discoverInstalledModules();

    EXPECT_TRUE(PluginManager::registry().knownPluginNames().empty());
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_DoesNotCrashWithNonexistentDir)
{
    PluginManager::setPluginsDir("/tmp/nonexistent_dir_12345");
    PluginManager::discoverInstalledModules();

    EXPECT_TRUE(PluginManager::registry().knownPluginNames().empty());
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_FindsFakeModulesWithoutCrash)
{
    std::string tmpDir = make_temp_dir();

    createFakeModule(tmpDir, "fake_module_a", "fake_module_a_plugin.so");
    createFakeModule(tmpDir, "fake_module_b", "fake_module_b_plugin.so");

    PluginManager::setPluginsDir(tmpDir.c_str());
    PluginManager::discoverInstalledModules();

    EXPECT_TRUE(PluginManager::registry().knownPluginNames().empty());
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_IgnoresModulesWithoutManifest)
{
    std::string tmpDir = make_temp_dir();

    const auto modDir = std::filesystem::path(tmpDir) / "no_manifest_module";
    std::filesystem::create_directories(modDir);
    std::ofstream bf(modDir / "plugin.so");
    bf << "fake";
    bf.close();

    PluginManager::setPluginsDir(tmpDir.c_str());
    PluginManager::discoverInstalledModules();

    EXPECT_TRUE(PluginManager::registry().knownPluginNames().empty());
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_IgnoresUiTypeModules)
{
    std::string tmpDir = make_temp_dir();

    createFakeModule(tmpDir, "ui_module", "ui_module_plugin.so", "ui");

    PluginManager::setPluginsDir(tmpDir.c_str());
    PluginManager::discoverInstalledModules();

    EXPECT_TRUE(PluginManager::registry().knownPluginNames().empty());
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_MultipleDirectories)
{
    std::string tmpDir1 = make_temp_dir();
    std::string tmpDir2 = make_temp_dir();

    createFakeModule(tmpDir1, "module_in_dir1", "module_in_dir1_plugin.so");
    createFakeModule(tmpDir2, "module_in_dir2", "module_in_dir2_plugin.so");

    PluginManager::setPluginsDir(tmpDir1.c_str());
    PluginManager::addPluginsDir(tmpDir2.c_str());

    ASSERT_EQ(PluginManager::registry().pluginsDirs().size(), 2u);

    PluginManager::discoverInstalledModules();

    EXPECT_TRUE(PluginManager::registry().knownPluginNames().empty());
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_InvalidManifestJson)
{
    std::string tmpDir = make_temp_dir();

    const auto modDir = std::filesystem::path(tmpDir) / "bad_manifest_module";
    std::filesystem::create_directories(modDir);
    std::ofstream mf(modDir / "manifest.json");
    mf << "{ this is not valid json }}}";
    mf.close();

    PluginManager::setPluginsDir(tmpDir.c_str());
    PluginManager::discoverInstalledModules();

    EXPECT_TRUE(PluginManager::registry().knownPluginNames().empty());
}
