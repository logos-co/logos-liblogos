#include <gtest/gtest.h>
#include "plugin_manager.h"
#include "logos_core.h"
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <cstring>

namespace {
QString testPlatformVariant() {
#if defined(Q_OS_MAC)
    #if defined(Q_PROCESSOR_ARM)
        return "darwin-arm64";
    #else
        return "darwin-x86_64";
    #endif
#elif defined(Q_OS_LINUX)
    #if defined(Q_PROCESSOR_X86_64)
        return "linux-x86_64";
    #elif defined(Q_PROCESSOR_ARM_64)
        return "linux-arm64";
    #else
        return "linux-x86";
    #endif
#else
        return "unknown";
#endif
}

QJsonObject mainFieldForPlatform(const QString &libName) {
    QString variant = testPlatformVariant();
#ifndef LOGOS_PORTABLE_BUILD
    variant += "-dev";
#endif
    return QJsonObject{{variant, libName}};
}
}

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
// findPlugins Function Tests
// =============================================================================

TEST_F(PluginManagerTest, FindPlugins_ReturnsEmptyForNonExistentDir) {
    QStringList plugins = PluginManager::findPlugins("/nonexistent/directory");
    EXPECT_TRUE(plugins.isEmpty());
}

TEST_F(PluginManagerTest, FindPlugins_ReturnsEmptyForEmptyDir) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    QStringList plugins = PluginManager::findPlugins(tempDir.path());
    EXPECT_TRUE(plugins.isEmpty());
}

TEST_F(PluginManagerTest, FindPlugins_DiscoversPluginFromManifest) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    QDir rootDir(tempDir.path());

    ASSERT_TRUE(rootDir.mkpath("my_plugin"));
    QDir pluginDir(rootDir.filePath("my_plugin"));

    QString libName = "my_plugin.dylib";
    QFile libFile(pluginDir.filePath(libName));
    ASSERT_TRUE(libFile.open(QIODevice::WriteOnly));
    libFile.close();

    QFile manifest(pluginDir.filePath("manifest.json"));
    ASSERT_TRUE(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(QJsonObject{{"main", mainFieldForPlatform(libName)}}).toJson());
    manifest.close();

    QStringList plugins = PluginManager::findPlugins(tempDir.path());

    ASSERT_EQ(plugins.size(), 1);
    EXPECT_TRUE(plugins[0].endsWith(libName));
}

TEST_F(PluginManagerTest, FindPlugins_SkipsSubdirWithoutManifest) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    QDir rootDir(tempDir.path());
    ASSERT_TRUE(rootDir.mkpath("no_manifest_plugin"));

    QFile libFile(rootDir.filePath("no_manifest_plugin/plugin.dylib"));
    ASSERT_TRUE(libFile.open(QIODevice::WriteOnly));
    libFile.close();

    QStringList plugins = PluginManager::findPlugins(tempDir.path());
    EXPECT_TRUE(plugins.isEmpty());
}

TEST_F(PluginManagerTest, FindPlugins_SkipsManifestWithoutMainField) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    QDir rootDir(tempDir.path());
    ASSERT_TRUE(rootDir.mkpath("bad_manifest_plugin"));

    QFile manifest(rootDir.filePath("bad_manifest_plugin/manifest.json"));
    ASSERT_TRUE(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(QJsonObject{{"version", "1.0"}}).toJson());
    manifest.close();

    QStringList plugins = PluginManager::findPlugins(tempDir.path());
    EXPECT_TRUE(plugins.isEmpty());
}

TEST_F(PluginManagerTest, FindPlugins_SkipsManifestWithWrongPlatform) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    QDir rootDir(tempDir.path());
    ASSERT_TRUE(rootDir.mkpath("wrong_platform_plugin"));
    QDir pluginDir(rootDir.filePath("wrong_platform_plugin"));

    QFile libFile(pluginDir.filePath("plugin.dll"));
    ASSERT_TRUE(libFile.open(QIODevice::WriteOnly));
    libFile.close();

    QJsonObject mainObj{{"fake-platform-999", "plugin.dll"}};
    QFile manifest(pluginDir.filePath("manifest.json"));
    ASSERT_TRUE(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(QJsonObject{{"main", mainObj}}).toJson());
    manifest.close();

    QStringList plugins = PluginManager::findPlugins(tempDir.path());
    EXPECT_TRUE(plugins.isEmpty());
}

TEST_F(PluginManagerTest, FindPlugins_SkipsManifestWithMissingLibrary) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    QDir rootDir(tempDir.path());
    ASSERT_TRUE(rootDir.mkpath("missing_lib_plugin"));

    QFile manifest(rootDir.filePath("missing_lib_plugin/manifest.json"));
    ASSERT_TRUE(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(QJsonObject{{"main", mainFieldForPlatform("nonexistent.dylib")}}).toJson());
    manifest.close();

    QStringList plugins = PluginManager::findPlugins(tempDir.path());
    EXPECT_TRUE(plugins.isEmpty());
}

TEST_F(PluginManagerTest, FindPlugins_DiscoversMultiplePlugins) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    QDir rootDir(tempDir.path());

    ASSERT_TRUE(rootDir.mkpath("plugin_a"));
    QFile libA(rootDir.filePath("plugin_a/liba.dylib"));
    ASSERT_TRUE(libA.open(QIODevice::WriteOnly));
    libA.close();
    QFile manifestA(rootDir.filePath("plugin_a/manifest.json"));
    ASSERT_TRUE(manifestA.open(QIODevice::WriteOnly));
    manifestA.write(QJsonDocument(QJsonObject{{"main", mainFieldForPlatform("liba.dylib")}}).toJson());
    manifestA.close();

    ASSERT_TRUE(rootDir.mkpath("plugin_b"));
    QFile libB(rootDir.filePath("plugin_b/libb.so"));
    ASSERT_TRUE(libB.open(QIODevice::WriteOnly));
    libB.close();
    QFile manifestB(rootDir.filePath("plugin_b/manifest.json"));
    ASSERT_TRUE(manifestB.open(QIODevice::WriteOnly));
    manifestB.write(QJsonDocument(QJsonObject{{"main", mainFieldForPlatform("libb.so")}}).toJson());
    manifestB.close();

    ASSERT_TRUE(rootDir.mkpath("plugin_c"));

    QStringList plugins = PluginManager::findPlugins(tempDir.path());

    ASSERT_EQ(plugins.size(), 2);
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
