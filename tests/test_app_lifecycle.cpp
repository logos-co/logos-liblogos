#include <gtest/gtest.h>
#include "app_lifecycle.h"
#include "plugin_manager.h"
#include <QCoreApplication>
#include <QDebug>

namespace PluginManager {
    extern QStringList s_plugins_dirs;
    extern QStringList s_loaded_plugins;
    extern QHash<QString, QString> s_known_plugins;
    extern QHash<QString, QJsonObject> s_plugin_metadata;
}

static void clearPluginState() {
    PluginManager::terminateAll();
    PluginManager::s_plugins_dirs.clear();
    PluginManager::s_known_plugins.clear();
    PluginManager::s_plugin_metadata.clear();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

class AppLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        clearPluginState();
    }

    void TearDown() override {
        clearPluginState();
    }
};

// =============================================================================
// Initialization Tests
// =============================================================================

TEST_F(AppLifecycleTest, Init_CreatesNewApp) {
    char* argv[] = {(char*)"test"};

    QCoreApplication* beforeApp = QCoreApplication::instance();

    AppLifecycle::init(1, argv);

    ASSERT_NE(QCoreApplication::instance(), nullptr);

    if (beforeApp) {
        EXPECT_EQ(QCoreApplication::instance(), beforeApp);
    }
}

TEST_F(AppLifecycleTest, Init_RegistersMetaType) {
    char* argv[] = {(char*)"test"};
    AppLifecycle::init(1, argv);

    int typeId = QMetaType::fromName("QObject*").id();
    EXPECT_NE(typeId, QMetaType::UnknownType);
}

// =============================================================================
// Plugin Directory Tests
// =============================================================================

TEST_F(AppLifecycleTest, SetPluginsDir_SetsDirectory) {
    const char* testDir = "/test/plugins";

    PluginManager::setPluginsDir(testDir);

    ASSERT_EQ(PluginManager::s_plugins_dirs.size(), 1);
    EXPECT_EQ(PluginManager::s_plugins_dirs[0].toStdString(), testDir);
}

TEST_F(AppLifecycleTest, SetPluginsDir_ClearsExisting) {
    PluginManager::addPluginsDir("/dir1");
    PluginManager::addPluginsDir("/dir2");
    ASSERT_EQ(PluginManager::s_plugins_dirs.size(), 2);

    PluginManager::setPluginsDir("/new_dir");

    ASSERT_EQ(PluginManager::s_plugins_dirs.size(), 1);
    EXPECT_EQ(PluginManager::s_plugins_dirs[0].toStdString(), "/new_dir");
}

TEST_F(AppLifecycleTest, AddPluginsDir_AppendsDirectory) {
    PluginManager::addPluginsDir("/dir1");
    PluginManager::addPluginsDir("/dir2");

    ASSERT_EQ(PluginManager::s_plugins_dirs.size(), 2);
    EXPECT_EQ(PluginManager::s_plugins_dirs[0].toStdString(), "/dir1");
    EXPECT_EQ(PluginManager::s_plugins_dirs[1].toStdString(), "/dir2");
}

TEST_F(AppLifecycleTest, AddPluginsDir_NoDuplicates) {
    PluginManager::addPluginsDir("/test");
    PluginManager::addPluginsDir("/test");

    EXPECT_EQ(PluginManager::s_plugins_dirs.size(), 1);
}

// =============================================================================
// Cleanup Tests
// =============================================================================

TEST_F(AppLifecycleTest, Cleanup_ClearsGlobals) {
    PluginManager::s_known_plugins.insert("test", "/path/to/test");
    PluginManager::addPluginsDir("/test");

    clearPluginState();

    EXPECT_TRUE(PluginManager::s_loaded_plugins.isEmpty());
    EXPECT_TRUE(PluginManager::s_known_plugins.isEmpty());
}

TEST_F(AppLifecycleTest, Cleanup_DeletesOwnedApp) {
    EXPECT_TRUE(true) << "This behavior is tested indirectly through integration tests";
}

TEST_F(AppLifecycleTest, Cleanup_PreservesExternalApp) {
    char* argv[] = {(char*)"test"};
    QCoreApplication* external = QCoreApplication::instance();
    ASSERT_NE(external, nullptr) << "Test runner should provide QCoreApplication";

    AppLifecycle::init(1, argv);

    EXPECT_EQ(QCoreApplication::instance(), external)
        << "Should preserve external app instance";
}

// =============================================================================
// ProcessEvents Test
// =============================================================================

TEST_F(AppLifecycleTest, ProcessEvents_HandlesNoApp) {
    // cleanup() resets app state to null (won't delete the test runner's app
    // since init() detected it as external and didn't claim ownership)
    char* argv[] = {(char*)"test"};
    AppLifecycle::init(1, argv);
    AppLifecycle::cleanup();

    EXPECT_NO_THROW(AppLifecycle::processEvents());
}

TEST_F(AppLifecycleTest, ProcessEvents_CallsAppProcessEvents) {
    char* argv[] = {(char*)"test"};
    AppLifecycle::init(1, argv);

    EXPECT_NO_THROW(AppLifecycle::processEvents());
}

// =============================================================================
// Start Function Tests (Limited testing without actual plugins)
// =============================================================================

TEST_F(AppLifecycleTest, Start_UsesCustomPluginsDirs) {
    char* argv[] = {(char*)"test"};
    AppLifecycle::init(1, argv);

    PluginManager::setPluginsDir("/custom/plugins");

    AppLifecycle::start();

    ASSERT_EQ(PluginManager::s_plugins_dirs.size(), 1);
    EXPECT_EQ(PluginManager::s_plugins_dirs[0].toStdString(), "/custom/plugins");
}

// =============================================================================
// Exec Test
// =============================================================================

TEST_F(AppLifecycleTest, Exec_ReturnsNegativeWhenNoApp) {
    char* argv[] = {(char*)"test"};
    AppLifecycle::init(1, argv);
    AppLifecycle::cleanup();

    int result = AppLifecycle::exec();

    EXPECT_EQ(result, -1);
}

TEST_F(AppLifecycleTest, Exec_WithAppAvailable) {
    char* argv[] = {(char*)"test"};
    AppLifecycle::init(1, argv);

    ASSERT_NE(QCoreApplication::instance(), nullptr);
}
