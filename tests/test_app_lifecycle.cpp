#include <gtest/gtest.h>
#include "app_lifecycle.h"
#include "plugin_manager.h"
#include "plugin_registry.h"

static void clearPluginState()
{
    PluginManager::terminateAll();
    PluginManager::registry().clear();
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

class AppLifecycleTest : public ::testing::Test {
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

TEST_F(AppLifecycleTest, Init_DoesNotThrow)
{
    char* argv[] = {(char*)"test"};
    EXPECT_NO_THROW(AppLifecycle::init(1, argv));
    AppLifecycle::cleanup();
}

TEST_F(AppLifecycleTest, SetPluginsDir_SetsDirectory)
{
    const char* testDir = "/test/plugins";

    PluginManager::setPluginsDir(testDir);

    ASSERT_EQ(PluginManager::registry().pluginsDirs().size(), 1u);
    EXPECT_EQ(PluginManager::registry().pluginsDirs()[0], testDir);
}

TEST_F(AppLifecycleTest, SetPluginsDir_ClearsExisting)
{
    PluginManager::addPluginsDir("/dir1");
    PluginManager::addPluginsDir("/dir2");
    ASSERT_EQ(PluginManager::registry().pluginsDirs().size(), 2u);

    PluginManager::setPluginsDir("/new_dir");

    ASSERT_EQ(PluginManager::registry().pluginsDirs().size(), 1u);
    EXPECT_EQ(PluginManager::registry().pluginsDirs()[0], "/new_dir");
}

TEST_F(AppLifecycleTest, AddPluginsDir_AppendsDirectory)
{
    PluginManager::addPluginsDir("/dir1");
    PluginManager::addPluginsDir("/dir2");

    ASSERT_EQ(PluginManager::registry().pluginsDirs().size(), 2u);
    EXPECT_EQ(PluginManager::registry().pluginsDirs()[0], "/dir1");
    EXPECT_EQ(PluginManager::registry().pluginsDirs()[1], "/dir2");
}

TEST_F(AppLifecycleTest, AddPluginsDir_NoDuplicates)
{
    PluginManager::addPluginsDir("/test");
    PluginManager::addPluginsDir("/test");

    EXPECT_EQ(PluginManager::registry().pluginsDirs().size(), 1u);
}

TEST_F(AppLifecycleTest, Cleanup_ClearsGlobals)
{
    PluginManager::registry().registerPlugin("test", "/path/to/test");
    PluginManager::addPluginsDir("/test");

    clearPluginState();

    EXPECT_TRUE(PluginManager::registry().loadedPluginNames().empty());
    EXPECT_TRUE(PluginManager::registry().knownPluginNames().empty());
}

TEST_F(AppLifecycleTest, ProcessEvents_HandlesNoApp)
{
    char* argv[] = {(char*)"test"};
    AppLifecycle::init(1, argv);
    AppLifecycle::cleanup();

    EXPECT_NO_THROW(AppLifecycle::processEvents());
}

TEST_F(AppLifecycleTest, ProcessEvents_DoesNotThrowAfterInit)
{
    char* argv[] = {(char*)"test"};
    AppLifecycle::init(1, argv);

    EXPECT_NO_THROW(AppLifecycle::processEvents());

    AppLifecycle::cleanup();
}

TEST_F(AppLifecycleTest, Start_UsesCustomPluginsDirs)
{
    char* argv[] = {(char*)"test"};
    AppLifecycle::init(1, argv);

    PluginManager::setPluginsDir("/custom/plugins");

    AppLifecycle::start();

    ASSERT_EQ(PluginManager::registry().pluginsDirs().size(), 1u);
    EXPECT_EQ(PluginManager::registry().pluginsDirs()[0], "/custom/plugins");

    AppLifecycle::cleanup();
}

TEST_F(AppLifecycleTest, Exec_ReturnsNegativeWhenNotInitialized)
{
    int result = AppLifecycle::exec();
    EXPECT_EQ(result, -1);
}

TEST_F(AppLifecycleTest, Exec_ReturnsNegativeAfterCleanup)
{
    char* argv[] = {(char*)"test"};
    AppLifecycle::init(1, argv);
    AppLifecycle::cleanup();

    int result = AppLifecycle::exec();

    EXPECT_EQ(result, -1);
}
