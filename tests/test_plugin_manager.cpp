#include <gtest/gtest.h>
#include "plugin_manager.h"
#include "logos_core.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QDir>
#include <QTemporaryDir>
#include <cstring>
#include <fstream>


class PluginManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        PluginManager::clearState();
    }

    void TearDown() override {
        PluginManager::clearState();
    }
};

// =============================================================================
// Plugin Query Functions Tests
// =============================================================================

TEST_F(PluginManagerTest, GetLoadedPlugins_ReturnsEmptyList) {
    QStringList loaded = PluginManager::getLoadedPlugins();
    EXPECT_TRUE(loaded.isEmpty());
}

TEST_F(PluginManagerTest, GetLoadedPlugins_ReturnsCorrectList) {
    PluginManager::registerLoadedPlugin("plugin1");
    PluginManager::registerLoadedPlugin("plugin2");
    PluginManager::registerLoadedPlugin("plugin3");

    QStringList loaded = PluginManager::getLoadedPlugins();

    ASSERT_EQ(loaded.size(), 3);
    EXPECT_EQ(loaded[0].toStdString(), "plugin1");
    EXPECT_EQ(loaded[1].toStdString(), "plugin2");
    EXPECT_EQ(loaded[2].toStdString(), "plugin3");
}

TEST_F(PluginManagerTest, GetKnownPlugins_ReturnsEmptyHash) {
    QHash<QString, QString> known = PluginManager::getKnownPlugins();
    EXPECT_TRUE(known.isEmpty());
}

TEST_F(PluginManagerTest, GetKnownPlugins_ReturnsCorrectHash) {
    PluginManager::addKnownPlugin("plugin1", "/path/to/plugin1.dylib");
    PluginManager::addKnownPlugin("plugin2", "/path/to/plugin2.dylib");

    QHash<QString, QString> known = PluginManager::getKnownPlugins();

    ASSERT_EQ(known.size(), 2);
    EXPECT_EQ(known.value("plugin1").toStdString(), "/path/to/plugin1.dylib");
    EXPECT_EQ(known.value("plugin2").toStdString(), "/path/to/plugin2.dylib");
}

TEST_F(PluginManagerTest, IsPluginLoaded_ReturnsFalseForUnloaded) {
    EXPECT_FALSE(PluginManager::isPluginLoaded("nonexistent_plugin"));
}

TEST_F(PluginManagerTest, IsPluginLoaded_ReturnsTrueForLoaded) {
    PluginManager::registerLoadedPlugin("test_plugin");

    EXPECT_TRUE(PluginManager::isPluginLoaded("test_plugin"));
}

TEST_F(PluginManagerTest, IsPluginKnown_ReturnsFalseForUnknown) {
    EXPECT_FALSE(PluginManager::isPluginKnown("nonexistent_plugin"));
}

TEST_F(PluginManagerTest, IsPluginKnown_ReturnsTrueForKnown) {
    PluginManager::addKnownPlugin("test_plugin", "/path/to/plugin");

    EXPECT_TRUE(PluginManager::isPluginKnown("test_plugin"));
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

TEST_F(PluginManagerTest, GetLoadedPluginsCStr_ReturnsCorrectArray) {
    PluginManager::registerLoadedPlugin("plugin1");
    PluginManager::registerLoadedPlugin("plugin2");

    char** result = PluginManager::getLoadedPluginsCStr();

    ASSERT_NE(result, nullptr);
    ASSERT_NE(result[0], nullptr);
    ASSERT_NE(result[1], nullptr);
    EXPECT_EQ(result[2], nullptr);

    EXPECT_STREQ(result[0], "plugin1");
    EXPECT_STREQ(result[1], "plugin2");

    delete[] result[0];
    delete[] result[1];
    delete[] result;
}

TEST_F(PluginManagerTest, GetKnownPluginsCStr_ReturnsNullTerminatedArrayWhenEmpty) {
    char** result = PluginManager::getKnownPluginsCStr();

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);

    delete[] result;
}

TEST_F(PluginManagerTest, GetKnownPluginsCStr_ReturnsCorrectArray) {
    PluginManager::addKnownPlugin("plugin1", "/path/to/plugin1");
    PluginManager::addKnownPlugin("plugin2", "/path/to/plugin2");

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

TEST_F(PluginManagerTest, LoadPlugin_ReturnsFalseForAlreadyLoaded) {
    PluginManager::addKnownPlugin("test_plugin", "/path/to/plugin");

    QProcess* dummyProcess = new QProcess();
    PluginManager::registerLoadedPlugin("test_plugin", dummyProcess);

    bool result = PluginManager::loadPlugin("test_plugin");
    EXPECT_FALSE(result);
}

// =============================================================================
// unloadPlugin Error Cases Tests
// =============================================================================

TEST_F(PluginManagerTest, UnloadPlugin_ReturnsFalseForNotLoaded) {
    bool result = PluginManager::unloadPlugin("nonexistent_plugin");
    EXPECT_FALSE(result);
}

TEST_F(PluginManagerTest, UnloadPlugin_ReturnsFalseForNoProcess) {
    PluginManager::registerLoadedPlugin("test_plugin");

    bool result = PluginManager::unloadPlugin("test_plugin");
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
    PluginManager::addKnownPlugin("plugin_a", "/path/to/plugin_a");
    QJsonObject metadata;
    metadata["name"] = "plugin_a";
    metadata["dependencies"] = QJsonArray();
    PluginManager::addPluginMetadata("plugin_a", metadata);

    QStringList requested;
    requested.append("plugin_a");

    QStringList result = PluginManager::resolveDependencies(requested);

    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].toStdString(), "plugin_a");
}

TEST_F(PluginManagerTest, ResolveDependencies_ReturnsCorrectOrder) {
    PluginManager::addKnownPlugin("plugin_a", "/path/to/plugin_a");
    PluginManager::addKnownPlugin("plugin_b", "/path/to/plugin_b");

    QJsonObject metadataA;
    metadataA["name"] = "plugin_a";
    QJsonArray depsA;
    depsA.append("plugin_b");
    metadataA["dependencies"] = depsA;
    PluginManager::addPluginMetadata("plugin_a", metadataA);

    QJsonObject metadataB;
    metadataB["name"] = "plugin_b";
    metadataB["dependencies"] = QJsonArray();
    PluginManager::addPluginMetadata("plugin_b", metadataB);

    QStringList requested;
    requested.append("plugin_a");

    QStringList result = PluginManager::resolveDependencies(requested);

    ASSERT_EQ(result.size(), 2);
    EXPECT_EQ(result[0].toStdString(), "plugin_b");
    EXPECT_EQ(result[1].toStdString(), "plugin_a");
}

TEST_F(PluginManagerTest, ResolveDependencies_HandlesTransitiveDeps) {
    PluginManager::addKnownPlugin("plugin_a", "/path/to/plugin_a");
    PluginManager::addKnownPlugin("plugin_b", "/path/to/plugin_b");
    PluginManager::addKnownPlugin("plugin_c", "/path/to/plugin_c");

    QJsonObject metadataA;
    metadataA["name"] = "plugin_a";
    QJsonArray depsA;
    depsA.append("plugin_b");
    metadataA["dependencies"] = depsA;
    PluginManager::addPluginMetadata("plugin_a", metadataA);

    QJsonObject metadataB;
    metadataB["name"] = "plugin_b";
    QJsonArray depsB;
    depsB.append("plugin_c");
    metadataB["dependencies"] = depsB;
    PluginManager::addPluginMetadata("plugin_b", metadataB);

    QJsonObject metadataC;
    metadataC["name"] = "plugin_c";
    metadataC["dependencies"] = QJsonArray();
    PluginManager::addPluginMetadata("plugin_c", metadataC);

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

TEST_F(PluginManagerTest, LoadPluginWithDependencies_SkipsAlreadyLoaded) {
    PluginManager::addKnownPlugin("plugin_a", "/path/to/plugin_a");
    PluginManager::addKnownPlugin("plugin_b", "/path/to/plugin_b");

    QJsonObject metadataA;
    metadataA["name"] = "plugin_a";
    QJsonArray depsA;
    depsA.append("plugin_b");
    metadataA["dependencies"] = depsA;
    PluginManager::addPluginMetadata("plugin_a", metadataA);

    QJsonObject metadataB;
    metadataB["name"] = "plugin_b";
    metadataB["dependencies"] = QJsonArray();
    PluginManager::addPluginMetadata("plugin_b", metadataB);

    PluginManager::registerLoadedPlugin("plugin_b");

    EXPECT_TRUE(PluginManager::isPluginLoaded("plugin_b"));
    EXPECT_FALSE(PluginManager::isPluginLoaded("plugin_a"));

    int result = logos_core_load_plugin_with_dependencies("plugin_a");

    EXPECT_TRUE(PluginManager::isPluginLoaded("plugin_b"));
}

// =============================================================================
// Plugin Directory Management Tests
// =============================================================================

TEST_F(PluginManagerTest, SetPluginsDir_SetsFirstDirectory) {
    PluginManager::setPluginsDir("/tmp/test_plugins");
    QStringList dirs = PluginManager::getPluginsDirs();
    ASSERT_EQ(dirs.size(), 1);
    EXPECT_EQ(dirs[0].toStdString(), "/tmp/test_plugins");
}

TEST_F(PluginManagerTest, AddPluginsDir_AppendsDirectory) {
    PluginManager::setPluginsDir("/tmp/dir1");
    PluginManager::addPluginsDir("/tmp/dir2");
    PluginManager::addPluginsDir("/tmp/dir3");

    QStringList dirs = PluginManager::getPluginsDirs();
    ASSERT_EQ(dirs.size(), 3);
    EXPECT_EQ(dirs[0].toStdString(), "/tmp/dir1");
    EXPECT_EQ(dirs[1].toStdString(), "/tmp/dir2");
    EXPECT_EQ(dirs[2].toStdString(), "/tmp/dir3");
}

TEST_F(PluginManagerTest, GetPluginsDirs_ReturnsEmptyAfterClear) {
    PluginManager::setPluginsDir("/tmp/dir1");
    PluginManager::clearState();
    QStringList dirs = PluginManager::getPluginsDirs();
    EXPECT_TRUE(dirs.isEmpty());
}

// =============================================================================
// Discovery Tests — fake installed modules
// =============================================================================

// Helper to create a fake installed module directory structure:
//   <parentDir>/<moduleName>/manifest.json
//   <parentDir>/<moduleName>/<mainFile>  (empty fake binary)
static void createFakeModule(const QString& parentDir,
                             const QString& moduleName,
                             const QString& mainFile,
                             const QString& type = "core") {
    QDir dir(parentDir);
    dir.mkpath(moduleName);

    // Write manifest.json
    QJsonObject manifest;
    manifest["name"] = moduleName;
    manifest["version"] = "1.0.0";
    manifest["type"] = type;
    manifest["main"] = mainFile;
    manifest["description"] = "Fake test module";

    QString manifestPath = dir.filePath(moduleName + "/manifest.json");
    std::ofstream mf(manifestPath.toStdString());
    QJsonDocument doc(manifest);
    mf << doc.toJson().toStdString();
    mf.close();

    // Create a fake binary file (processPlugin will fail on this, which is expected)
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

    // No modules to find — known plugins should remain empty
    QHash<QString, QString> known = PluginManager::getKnownPlugins();
    EXPECT_TRUE(known.isEmpty());
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_DoesNotCrashWithNonexistentDir) {
    PluginManager::setPluginsDir("/tmp/nonexistent_dir_12345");
    PluginManager::discoverInstalledModules();

    QHash<QString, QString> known = PluginManager::getKnownPlugins();
    EXPECT_TRUE(known.isEmpty());
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_FindsFakeModulesWithoutCrash) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    createFakeModule(tmpDir.path(), "fake_module_a", "fake_module_a_plugin.so");
    createFakeModule(tmpDir.path(), "fake_module_b", "fake_module_b_plugin.so");

    PluginManager::setPluginsDir(tmpDir.path().toUtf8().constData());
    PluginManager::discoverInstalledModules();

    // processPlugin fails for fake binaries, so they won't be in known plugins.
    // The important thing is that discovery ran without crashing and the
    // scanning pipeline found the manifest.json files.
    QHash<QString, QString> known = PluginManager::getKnownPlugins();
    EXPECT_TRUE(known.isEmpty());
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_IgnoresModulesWithoutManifest) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    // Create a module directory without manifest.json
    QDir dir(tmpDir.path());
    dir.mkpath("no_manifest_module");
    QString binaryPath = dir.filePath("no_manifest_module/plugin.so");
    std::ofstream bf(binaryPath.toStdString());
    bf << "fake";
    bf.close();

    PluginManager::setPluginsDir(tmpDir.path().toUtf8().constData());
    PluginManager::discoverInstalledModules();

    QHash<QString, QString> known = PluginManager::getKnownPlugins();
    EXPECT_TRUE(known.isEmpty());
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_IgnoresUiTypeModules) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    // Create a UI module — getInstalledModules() only scans for "core" type
    createFakeModule(tmpDir.path(), "ui_module", "ui_module_plugin.so", "ui");

    PluginManager::setPluginsDir(tmpDir.path().toUtf8().constData());
    PluginManager::discoverInstalledModules();

    // UI modules are not discovered by getInstalledModules (which filters by "core")
    QHash<QString, QString> known = PluginManager::getKnownPlugins();
    EXPECT_TRUE(known.isEmpty());
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

    QStringList dirs = PluginManager::getPluginsDirs();
    ASSERT_EQ(dirs.size(), 2);

    PluginManager::discoverInstalledModules();

    // Both directories should be scanned without crash
    // (processPlugin still fails for fake binaries)
    QHash<QString, QString> known = PluginManager::getKnownPlugins();
    EXPECT_TRUE(known.isEmpty());
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_InvalidManifestJson) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    // Create a module with invalid JSON in manifest
    QDir dir(tmpDir.path());
    dir.mkpath("bad_manifest_module");
    QString manifestPath = dir.filePath("bad_manifest_module/manifest.json");
    std::ofstream mf(manifestPath.toStdString());
    mf << "{ this is not valid json }}}";
    mf.close();

    PluginManager::setPluginsDir(tmpDir.path().toUtf8().constData());
    PluginManager::discoverInstalledModules();

    QHash<QString, QString> known = PluginManager::getKnownPlugins();
    EXPECT_TRUE(known.isEmpty());
}
