#include <gtest/gtest.h>
#include "plugin_manager.h"
#include "plugin_registry.h"
#include "logos_core.h"
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <nlohmann/json.hpp>
#include <cstring>
#include <fstream>

static void clearPluginState() {
    PluginManager::terminateAll();
    PluginManager::registry().clear();
}

class PluginManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        clearPluginState();
    }

    void TearDown() override {
        clearPluginState();
    }
};

// =============================================================================
// Plugin Query Functions Tests
// =============================================================================

TEST_F(PluginManagerTest, GetLoadedPlugins_ReturnsEmptyList) {
    EXPECT_TRUE(PluginManager::registry().loadedPluginNames().isEmpty());
}

TEST_F(PluginManagerTest, GetKnownPlugins_ReturnsEmptyHash) {
    EXPECT_TRUE(PluginManager::registry().knownPluginNames().isEmpty());
}

TEST_F(PluginManagerTest, GetKnownPlugins_ReturnsCorrectHash) {
    PluginManager::registry().registerPlugin("plugin1", "/path/to/plugin1.dylib");
    PluginManager::registry().registerPlugin("plugin2", "/path/to/plugin2.dylib");

    ASSERT_EQ(PluginManager::registry().knownPluginNames().size(), 2);
    EXPECT_EQ(PluginManager::registry().pluginPath("plugin1").toStdString(), "/path/to/plugin1.dylib");
    EXPECT_EQ(PluginManager::registry().pluginPath("plugin2").toStdString(), "/path/to/plugin2.dylib");
}

TEST_F(PluginManagerTest, IsPluginLoaded_ReturnsFalseForUnloaded) {
    EXPECT_FALSE(PluginManager::isPluginLoaded("nonexistent_plugin"));
}

TEST_F(PluginManagerTest, IsPluginKnown_ReturnsFalseForUnknown) {
    EXPECT_FALSE(PluginManager::registry().isKnown("nonexistent_plugin"));
}

TEST_F(PluginManagerTest, IsPluginKnown_ReturnsTrueForKnown) {
    PluginManager::registry().registerPlugin("test_plugin", "/path/to/plugin");

    EXPECT_TRUE(PluginManager::registry().isKnown("test_plugin"));
}

// =============================================================================
// C String Array Functions Tests
// =============================================================================

TEST_F(PluginManagerTest, GetLoadedPluginsCStr_ReturnsNullTerminatedArrayWhenEmpty) {
    char** result = PluginManager::getLoadedPluginsCStr();

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);

    delete[] result;
}

TEST_F(PluginManagerTest, GetKnownPluginsCStr_ReturnsNullTerminatedArrayWhenEmpty) {
    char** result = PluginManager::getKnownPluginsCStr();

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);

    delete[] result;
}

TEST_F(PluginManagerTest, GetKnownPluginsCStr_ReturnsCorrectArray) {
    PluginManager::registry().registerPlugin("plugin1", "/path/to/plugin1");
    PluginManager::registry().registerPlugin("plugin2", "/path/to/plugin2");

    char** result = PluginManager::getKnownPluginsCStr();

    ASSERT_NE(result, nullptr);
    ASSERT_NE(result[0], nullptr);
    ASSERT_NE(result[1], nullptr);
    EXPECT_EQ(result[2], nullptr);

    QStringList plugins;
    plugins.append(QString::fromUtf8(result[0]));
    plugins.append(QString::fromUtf8(result[1]));

    EXPECT_TRUE(plugins.contains("plugin1"));
    EXPECT_TRUE(plugins.contains("plugin2"));

    delete[] result[0];
    delete[] result[1];
    delete[] result;
}

// =============================================================================
// loadPlugin Error Cases Tests
// =============================================================================

TEST_F(PluginManagerTest, LoadPlugin_ReturnsFalseForUnknownPlugin) {
    bool result = PluginManager::loadPlugin("nonexistent_plugin");
    EXPECT_FALSE(result);
}

// =============================================================================
// unloadPlugin Error Cases Tests
// =============================================================================

TEST_F(PluginManagerTest, UnloadPlugin_ReturnsFalseForNotLoaded) {
    bool result = PluginManager::unloadPlugin("nonexistent_plugin");
    EXPECT_FALSE(result);
}

// =============================================================================
// resolveDependencies Function Tests
// =============================================================================

TEST_F(PluginManagerTest, ResolveDependencies_ReturnsEmptyForEmptyInput) {
    QStringList result = PluginManager::resolveDependencies(QStringList());
    EXPECT_TRUE(result.isEmpty());
}

TEST_F(PluginManagerTest, ResolveDependencies_ReturnsEmptyForUnknownPlugin) {
    QStringList requested;
    requested.append("unknown_plugin");

    QStringList result = PluginManager::resolveDependencies(requested);
    EXPECT_TRUE(result.isEmpty());
}

TEST_F(PluginManagerTest, ResolveDependencies_ReturnsSinglePluginWithNoDeps) {
    PluginManager::registry().registerPlugin("plugin_a", "/path/to/plugin_a");
    nlohmann::json metadata;
    metadata["name"] = "plugin_a";
    metadata["dependencies"] = nlohmann::json::array();
    PluginManager::registry().registerMetadata("plugin_a", metadata);

    QStringList requested;
    requested.append("plugin_a");

    QStringList result = PluginManager::resolveDependencies(requested);

    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].toStdString(), "plugin_a");
}

TEST_F(PluginManagerTest, ResolveDependencies_ReturnsCorrectOrder) {
    PluginManager::registry().registerPlugin("plugin_a", "/path/to/plugin_a");
    PluginManager::registry().registerPlugin("plugin_b", "/path/to/plugin_b");

    nlohmann::json metadataA;
    metadataA["name"] = "plugin_a";
    metadataA["dependencies"] = nlohmann::json::array({"plugin_b"});
    PluginManager::registry().registerMetadata("plugin_a", metadataA);

    nlohmann::json metadataB;
    metadataB["name"] = "plugin_b";
    metadataB["dependencies"] = nlohmann::json::array();
    PluginManager::registry().registerMetadata("plugin_b", metadataB);

    QStringList requested;
    requested.append("plugin_a");

    QStringList result = PluginManager::resolveDependencies(requested);

    ASSERT_EQ(result.size(), 2);
    EXPECT_EQ(result[0].toStdString(), "plugin_b");
    EXPECT_EQ(result[1].toStdString(), "plugin_a");
}

TEST_F(PluginManagerTest, ResolveDependencies_HandlesTransitiveDeps) {
    PluginManager::registry().registerPlugin("plugin_a", "/path/to/plugin_a");
    PluginManager::registry().registerPlugin("plugin_b", "/path/to/plugin_b");
    PluginManager::registry().registerPlugin("plugin_c", "/path/to/plugin_c");

    nlohmann::json metadataA;
    metadataA["name"] = "plugin_a";
    metadataA["dependencies"] = nlohmann::json::array({"plugin_b"});
    PluginManager::registry().registerMetadata("plugin_a", metadataA);

    nlohmann::json metadataB;
    metadataB["name"] = "plugin_b";
    metadataB["dependencies"] = nlohmann::json::array({"plugin_c"});
    PluginManager::registry().registerMetadata("plugin_b", metadataB);

    nlohmann::json metadataC;
    metadataC["name"] = "plugin_c";
    metadataC["dependencies"] = nlohmann::json::array();
    PluginManager::registry().registerMetadata("plugin_c", metadataC);

    QStringList requested;
    requested.append("plugin_a");

    QStringList result = PluginManager::resolveDependencies(requested);

    ASSERT_EQ(result.size(), 3);
    EXPECT_EQ(result[0].toStdString(), "plugin_c");
    EXPECT_EQ(result[1].toStdString(), "plugin_b");
    EXPECT_EQ(result[2].toStdString(), "plugin_a");
}

// =============================================================================
// C API: logos_core_load_plugin_with_dependencies Tests
// =============================================================================

TEST_F(PluginManagerTest, LoadPluginWithDependencies_AbortsForNull) {
    EXPECT_DEATH(logos_core_load_plugin_with_dependencies(nullptr), "");
}

TEST_F(PluginManagerTest, LoadPluginWithDependencies_ReturnsZeroForUnknown) {
    int result = logos_core_load_plugin_with_dependencies("unknown_plugin");
    EXPECT_EQ(result, 0);
}

// =============================================================================
// Plugin Directory Management Tests
// =============================================================================

TEST_F(PluginManagerTest, SetPluginsDir_SetsFirstDirectory) {
    PluginManager::setPluginsDir("/tmp/test_plugins");
    ASSERT_EQ(PluginManager::registry().pluginsDirs().size(), 1);
    EXPECT_EQ(PluginManager::registry().pluginsDirs()[0].toStdString(), "/tmp/test_plugins");
}

TEST_F(PluginManagerTest, AddPluginsDir_AppendsDirectory) {
    PluginManager::setPluginsDir("/tmp/dir1");
    PluginManager::addPluginsDir("/tmp/dir2");
    PluginManager::addPluginsDir("/tmp/dir3");

    ASSERT_EQ(PluginManager::registry().pluginsDirs().size(), 3);
    EXPECT_EQ(PluginManager::registry().pluginsDirs()[0].toStdString(), "/tmp/dir1");
    EXPECT_EQ(PluginManager::registry().pluginsDirs()[1].toStdString(), "/tmp/dir2");
    EXPECT_EQ(PluginManager::registry().pluginsDirs()[2].toStdString(), "/tmp/dir3");
}

TEST_F(PluginManagerTest, GetPluginsDirs_ReturnsEmptyAfterClear) {
    PluginManager::setPluginsDir("/tmp/dir1");
    clearPluginState();
    EXPECT_TRUE(PluginManager::registry().pluginsDirs().isEmpty());
}

// =============================================================================
// Discovery Tests — fake installed modules
// =============================================================================

static void createFakeModule(const QString& parentDir,
                             const QString& moduleName,
                             const QString& mainFile,
                             const QString& type = "core") {
    QDir dir(parentDir);
    dir.mkpath(moduleName);

    nlohmann::json manifest;
    manifest["name"] = moduleName.toStdString();
    manifest["version"] = "1.0.0";
    manifest["type"] = type.toStdString();
    manifest["main"] = mainFile.toStdString();
    manifest["description"] = "Fake test module";

    QString manifestPath = dir.filePath(moduleName + "/manifest.json");
    std::ofstream mf(manifestPath.toStdString());
    mf << manifest.dump();
    mf.close();

    QString binaryPath = dir.filePath(moduleName + "/" + mainFile);
    std::ofstream bf(binaryPath.toStdString());
    bf << "fake";
    bf.close();
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_DoesNotCrashWithEmptyDir) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    PluginManager::setPluginsDir(tmpDir.path().toUtf8().constData());
    PluginManager::discoverInstalledModules();

    EXPECT_TRUE(PluginManager::registry().knownPluginNames().isEmpty());
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_DoesNotCrashWithNonexistentDir) {
    PluginManager::setPluginsDir("/tmp/nonexistent_dir_12345");
    PluginManager::discoverInstalledModules();

    EXPECT_TRUE(PluginManager::registry().knownPluginNames().isEmpty());
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_FindsFakeModulesWithoutCrash) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    createFakeModule(tmpDir.path(), "fake_module_a", "fake_module_a_plugin.so");
    createFakeModule(tmpDir.path(), "fake_module_b", "fake_module_b_plugin.so");

    PluginManager::setPluginsDir(tmpDir.path().toUtf8().constData());
    PluginManager::discoverInstalledModules();

    EXPECT_TRUE(PluginManager::registry().knownPluginNames().isEmpty());
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_IgnoresModulesWithoutManifest) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QDir dir(tmpDir.path());
    dir.mkpath("no_manifest_module");
    QString binaryPath = dir.filePath("no_manifest_module/plugin.so");
    std::ofstream bf(binaryPath.toStdString());
    bf << "fake";
    bf.close();

    PluginManager::setPluginsDir(tmpDir.path().toUtf8().constData());
    PluginManager::discoverInstalledModules();

    EXPECT_TRUE(PluginManager::registry().knownPluginNames().isEmpty());
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_IgnoresUiTypeModules) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    createFakeModule(tmpDir.path(), "ui_module", "ui_module_plugin.so", "ui");

    PluginManager::setPluginsDir(tmpDir.path().toUtf8().constData());
    PluginManager::discoverInstalledModules();

    EXPECT_TRUE(PluginManager::registry().knownPluginNames().isEmpty());
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_MultipleDirectories) {
    QTemporaryDir tmpDir1;
    QTemporaryDir tmpDir2;
    ASSERT_TRUE(tmpDir1.isValid());
    ASSERT_TRUE(tmpDir2.isValid());

    createFakeModule(tmpDir1.path(), "module_in_dir1", "module_in_dir1_plugin.so");
    createFakeModule(tmpDir2.path(), "module_in_dir2", "module_in_dir2_plugin.so");

    PluginManager::setPluginsDir(tmpDir1.path().toUtf8().constData());
    PluginManager::addPluginsDir(tmpDir2.path().toUtf8().constData());

    ASSERT_EQ(PluginManager::registry().pluginsDirs().size(), 2);

    PluginManager::discoverInstalledModules();

    EXPECT_TRUE(PluginManager::registry().knownPluginNames().isEmpty());
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_InvalidManifestJson) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QDir dir(tmpDir.path());
    dir.mkpath("bad_manifest_module");
    QString manifestPath = dir.filePath("bad_manifest_module/manifest.json");
    std::ofstream mf(manifestPath.toStdString());
    mf << "{ this is not valid json }}}";
    mf.close();

    PluginManager::setPluginsDir(tmpDir.path().toUtf8().constData());
    PluginManager::discoverInstalledModules();

    EXPECT_TRUE(PluginManager::registry().knownPluginNames().isEmpty());
}
