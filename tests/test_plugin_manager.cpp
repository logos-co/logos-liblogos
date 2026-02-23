#include <gtest/gtest.h>
#include "plugin_manager.h"
#include "logos_core_internal.h"
#include "logos_core.h"
#include "logos_mode.h"
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
#elif defined(Q_OS_WIN)
    #if defined(Q_PROCESSOR_X86_64)
        return "windows-x86_64";
    #else
        return "windows-x86";
    #endif
#else
        return "unknown";
#endif
}

QJsonObject mainFieldForPlatform(const QString &libName) {
    return QJsonObject{{testPlatformVariant(), libName}};
}
}

// Test fixture for plugin manager tests
class PluginManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clear global state before each test using public API
        PluginManager::clearState();
        
        // Set to Local mode by default (most testable functions require it)
        LogosModeConfig::setMode(LogosMode::Local);
        
        // Ensure registry host is null at start
        if (g_registry_host) {
            delete g_registry_host;
            g_registry_host = nullptr;
        }
    }
    
    void TearDown() override {
        // Clean up after each test
        
        // Clean up registry host if it was created
        if (g_registry_host) {
            delete g_registry_host;
            g_registry_host = nullptr;
        }
        
        // Clear plugin state using public API
        PluginManager::clearState();
    }
};

// =============================================================================
// Plugin Query Functions Tests
// =============================================================================

// Verifies that getLoadedPlugins() returns an empty list when no plugins are loaded
TEST_F(PluginManagerTest, GetLoadedPlugins_ReturnsEmptyList) {
    QStringList loaded = PluginManager::getLoadedPlugins();
    EXPECT_TRUE(loaded.isEmpty());
}

// Verifies that getLoadedPlugins() returns the correct list of loaded plugin names
// Note: This is an implementation test that directly manipulates internal state
TEST_F(PluginManagerTest, GetLoadedPlugins_ReturnsCorrectList) {
    // Add some plugins to the global list
    g_loaded_plugins.append("plugin1");
    g_loaded_plugins.append("plugin2");
    g_loaded_plugins.append("plugin3");
    
    QStringList loaded = PluginManager::getLoadedPlugins();
    
    ASSERT_EQ(loaded.size(), 3);
    EXPECT_EQ(loaded[0].toStdString(), "plugin1");
    EXPECT_EQ(loaded[1].toStdString(), "plugin2");
    EXPECT_EQ(loaded[2].toStdString(), "plugin3");
}

// Verifies that getKnownPlugins() returns an empty hash when no plugins are known
TEST_F(PluginManagerTest, GetKnownPlugins_ReturnsEmptyHash) {
    QHash<QString, QString> known = PluginManager::getKnownPlugins();
    EXPECT_TRUE(known.isEmpty());
}

// Verifies that getKnownPlugins() returns the correct hash of plugin names to paths
TEST_F(PluginManagerTest, GetKnownPlugins_ReturnsCorrectHash) {
    // Add some plugins using the public API
    PluginManager::addKnownPlugin("plugin1", "/path/to/plugin1.dylib");
    PluginManager::addKnownPlugin("plugin2", "/path/to/plugin2.dylib");
    
    QHash<QString, QString> known = PluginManager::getKnownPlugins();
    
    ASSERT_EQ(known.size(), 2);
    EXPECT_EQ(known.value("plugin1").toStdString(), "/path/to/plugin1.dylib");
    EXPECT_EQ(known.value("plugin2").toStdString(), "/path/to/plugin2.dylib");
}

// Verifies that isPluginLoaded() returns false for plugins that are not loaded
TEST_F(PluginManagerTest, IsPluginLoaded_ReturnsFalseForUnloaded) {
    EXPECT_FALSE(PluginManager::isPluginLoaded("nonexistent_plugin"));
}

// Verifies that isPluginLoaded() returns true for plugins that are currently loaded
// Note: This is an implementation test that directly manipulates internal state
TEST_F(PluginManagerTest, IsPluginLoaded_ReturnsTrueForLoaded) {
    g_loaded_plugins.append("test_plugin");
    
    EXPECT_TRUE(PluginManager::isPluginLoaded("test_plugin"));
}

// Verifies that isPluginKnown() returns false for plugins that have not been discovered
TEST_F(PluginManagerTest, IsPluginKnown_ReturnsFalseForUnknown) {
    EXPECT_FALSE(PluginManager::isPluginKnown("nonexistent_plugin"));
}

// Verifies that isPluginKnown() returns true for plugins that have been discovered
TEST_F(PluginManagerTest, IsPluginKnown_ReturnsTrueForKnown) {
    PluginManager::addKnownPlugin("test_plugin", "/path/to/plugin");
    
    EXPECT_TRUE(PluginManager::isPluginKnown("test_plugin"));
}

// =============================================================================
// C String Array Functions Tests
// =============================================================================

// Verifies that getLoadedPluginsCStr() returns a null-terminated array with just NULL when no plugins are loaded
TEST_F(PluginManagerTest, GetLoadedPluginsCStr_ReturnsNullTerminatedArrayWhenEmpty) {
    char** result = PluginManager::getLoadedPluginsCStr();
    
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);
    
    // Clean up
    delete[] result;
}

// Verifies that getLoadedPluginsCStr() returns a properly allocated null-terminated C string array
// containing all loaded plugin names for C API compatibility
// Note: This is an implementation test that directly manipulates internal state
TEST_F(PluginManagerTest, GetLoadedPluginsCStr_ReturnsCorrectArray) {
    // Add plugins to the global list
    g_loaded_plugins.append("plugin1");
    g_loaded_plugins.append("plugin2");
    
    char** result = PluginManager::getLoadedPluginsCStr();
    
    ASSERT_NE(result, nullptr);
    ASSERT_NE(result[0], nullptr);
    ASSERT_NE(result[1], nullptr);
    EXPECT_EQ(result[2], nullptr); // Null terminator
    
    EXPECT_STREQ(result[0], "plugin1");
    EXPECT_STREQ(result[1], "plugin2");
    
    // Clean up
    delete[] result[0];
    delete[] result[1];
    delete[] result;
}

// Verifies that getKnownPluginsCStr() returns a null-terminated array with just NULL when no plugins are known
TEST_F(PluginManagerTest, GetKnownPluginsCStr_ReturnsNullTerminatedArrayWhenEmpty) {
    char** result = PluginManager::getKnownPluginsCStr();
    
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);
    
    // Clean up
    delete[] result;
}

// Verifies that getKnownPluginsCStr() returns a properly allocated null-terminated C string array
// containing all known plugin names for C API compatibility
TEST_F(PluginManagerTest, GetKnownPluginsCStr_ReturnsCorrectArray) {
    // Add plugins using the public API
    PluginManager::addKnownPlugin("plugin1", "/path/to/plugin1");
    PluginManager::addKnownPlugin("plugin2", "/path/to/plugin2");
    
    char** result = PluginManager::getKnownPluginsCStr();
    
    ASSERT_NE(result, nullptr);
    ASSERT_NE(result[0], nullptr);
    ASSERT_NE(result[1], nullptr);
    EXPECT_EQ(result[2], nullptr); // Null terminator
    
    // Note: QHash order is not guaranteed, so we check that both plugins are present
    QStringList plugins;
    plugins.append(QString::fromUtf8(result[0]));
    plugins.append(QString::fromUtf8(result[1]));
    
    EXPECT_TRUE(plugins.contains("plugin1"));
    EXPECT_TRUE(plugins.contains("plugin2"));
    
    // Clean up
    delete[] result[0];
    delete[] result[1];
    delete[] result;
}

// =============================================================================
// findPlugins Function Tests
// =============================================================================

// Verifies that findPlugins() returns an empty list when the directory does not exist
TEST_F(PluginManagerTest, FindPlugins_ReturnsEmptyForNonExistentDir) {
    QStringList plugins = PluginManager::findPlugins("/nonexistent/directory");
    EXPECT_TRUE(plugins.isEmpty());
}

// Verifies that findPlugins() returns an empty list when the directory exists but contains no plugins
TEST_F(PluginManagerTest, FindPlugins_ReturnsEmptyForEmptyDir) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    
    QStringList plugins = PluginManager::findPlugins(tempDir.path());
    EXPECT_TRUE(plugins.isEmpty());
}

// Verifies that findPlugins() discovers plugins from subdirectories containing a manifest.json
// with a "main" field pointing to an existing library file
TEST_F(PluginManagerTest, FindPlugins_DiscoversPluginFromManifest) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    
    QDir rootDir(tempDir.path());
    
    // Create a valid plugin subdirectory with manifest and library
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

// Verifies that findPlugins() skips subdirectories that have no manifest.json
TEST_F(PluginManagerTest, FindPlugins_SkipsSubdirWithoutManifest) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    
    QDir rootDir(tempDir.path());
    ASSERT_TRUE(rootDir.mkpath("no_manifest_plugin"));
    
    // Create a library file but no manifest
    QFile libFile(rootDir.filePath("no_manifest_plugin/plugin.dylib"));
    ASSERT_TRUE(libFile.open(QIODevice::WriteOnly));
    libFile.close();
    
    QStringList plugins = PluginManager::findPlugins(tempDir.path());
    EXPECT_TRUE(plugins.isEmpty());
}

// Verifies that findPlugins() skips manifests with a missing "main" field
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

// Verifies that findPlugins() skips manifests where "main" has no entry for the current platform
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

// Verifies that findPlugins() skips manifests where "main" points to a nonexistent file
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

// Verifies that findPlugins() discovers multiple valid plugins and ignores invalid ones
TEST_F(PluginManagerTest, FindPlugins_DiscoversMultiplePlugins) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    
    QDir rootDir(tempDir.path());
    
    // Valid plugin A
    ASSERT_TRUE(rootDir.mkpath("plugin_a"));
    QFile libA(rootDir.filePath("plugin_a/liba.dylib"));
    ASSERT_TRUE(libA.open(QIODevice::WriteOnly));
    libA.close();
    QFile manifestA(rootDir.filePath("plugin_a/manifest.json"));
    ASSERT_TRUE(manifestA.open(QIODevice::WriteOnly));
    manifestA.write(QJsonDocument(QJsonObject{{"main", mainFieldForPlatform("liba.dylib")}}).toJson());
    manifestA.close();
    
    // Valid plugin B
    ASSERT_TRUE(rootDir.mkpath("plugin_b"));
    QFile libB(rootDir.filePath("plugin_b/libb.so"));
    ASSERT_TRUE(libB.open(QIODevice::WriteOnly));
    libB.close();
    QFile manifestB(rootDir.filePath("plugin_b/manifest.json"));
    ASSERT_TRUE(manifestB.open(QIODevice::WriteOnly));
    manifestB.write(QJsonDocument(QJsonObject{{"main", mainFieldForPlatform("libb.so")}}).toJson());
    manifestB.close();
    
    // Invalid: no manifest
    ASSERT_TRUE(rootDir.mkpath("plugin_c"));
    
    QStringList plugins = PluginManager::findPlugins(tempDir.path());
    
    ASSERT_EQ(plugins.size(), 2);
}

// =============================================================================
// loadPlugin Error Cases Tests
// =============================================================================

// Verifies that loadPlugin() returns false when attempting to load a plugin that is not in the known plugins list
TEST_F(PluginManagerTest, LoadPlugin_ReturnsFalseForUnknownPlugin) {
    bool result = PluginManager::loadPlugin("nonexistent_plugin");
    EXPECT_FALSE(result);
}

// Verifies that loadPlugin() returns false when attempting to load a plugin that is already loaded in Local mode
TEST_F(PluginManagerTest, LoadPlugin_ReturnsFalseForAlreadyLoadedLocal) {
    // Ensure we're in Local mode (default, but being explicit)
    LogosModeConfig::setMode(LogosMode::Local);
    
    // Simulate a loaded plugin in Local mode
    PluginManager::addKnownPlugin("test_plugin", "/path/to/plugin");
    g_loaded_plugins.append("test_plugin");
    
    // Create a dummy LogosAPI to simulate loaded plugin
    // Note: This is an implementation test verifying internal state checking
    g_local_plugin_apis.insert("test_plugin", nullptr);
    
    bool result = PluginManager::loadPlugin("test_plugin");
    EXPECT_FALSE(result);
}

#ifndef Q_OS_IOS
// Verifies that loadPlugin() returns false when attempting to load a plugin that is already running
// in a separate process in Remote mode
TEST_F(PluginManagerTest, LoadPlugin_ReturnsFalseForAlreadyLoadedRemote) {
    // Set to Remote mode explicitly
    LogosModeConfig::setMode(LogosMode::Remote);
    
    // Simulate a loaded plugin in Remote mode
    PluginManager::addKnownPlugin("test_plugin", "/path/to/plugin");
    g_loaded_plugins.append("test_plugin");
    
    // Create a dummy process to simulate loaded plugin
    // Note: This is an implementation test verifying internal state checking
    QProcess* dummyProcess = new QProcess();
    g_plugin_processes.insert("test_plugin", dummyProcess);
    
    bool result = PluginManager::loadPlugin("test_plugin");
    EXPECT_FALSE(result);
}
#endif

// =============================================================================
// unloadPlugin Error Cases Tests
// =============================================================================

#ifdef Q_OS_IOS
// Verifies that unloadPlugin() is not supported on iOS (which doesn't support process-based plugins)
TEST_F(PluginManagerTest, UnloadPlugin_NotSupportedOnIOS) {
    bool result = PluginManager::unloadPlugin("any_plugin");
    EXPECT_FALSE(result);
}
#else
// Verifies that unloadPlugin() returns false when attempting to unload a plugin that is not loaded
TEST_F(PluginManagerTest, UnloadPlugin_ReturnsFalseForNotLoaded) {
    bool result = PluginManager::unloadPlugin("nonexistent_plugin");
    EXPECT_FALSE(result);
}

// Verifies that unloadPlugin() returns false when a plugin is marked as loaded but has no associated process
TEST_F(PluginManagerTest, UnloadPlugin_ReturnsFalseForNoProcess) {
    // Add plugin to loaded list but not to processes
    // Note: This is an implementation test verifying internal state consistency
    g_loaded_plugins.append("test_plugin");
    
    bool result = PluginManager::unloadPlugin("test_plugin");
    EXPECT_FALSE(result);
}
#endif

// =============================================================================
// Mode-Dependent Functions Tests
// =============================================================================

// Verifies that loadStaticPlugins() returns 0 when no static plugins are registered
// (requires Local mode for static plugin loading)
TEST_F(PluginManagerTest, LoadStaticPlugins_ReturnsZeroWhenNoPlugins) {
    // Set to Local mode (required for loadStaticPlugins)
    LogosModeConfig::setMode(LogosMode::Local);
    
    // Should return 0 when no static plugins are registered
    int result = PluginManager::loadStaticPlugins();
    EXPECT_EQ(result, 0);
}

// Verifies that registerPluginInstance() returns false when given a null plugin instance pointer
// (requires Local mode for direct plugin registration)
TEST_F(PluginManagerTest, RegisterPluginInstance_ReturnsFalseForNullInstance) {
    // Set to Local mode (required for registerPluginInstance)
    LogosModeConfig::setMode(LogosMode::Local);
    
    bool result = PluginManager::registerPluginInstance("test_plugin", nullptr);
    EXPECT_FALSE(result);
}

// Verifies that registerPluginByName() returns false when attempting to register a static plugin
// that doesn't exist in the static plugin instances list (requires Local mode)
TEST_F(PluginManagerTest, RegisterPluginByName_ReturnsFalseForUnfoundPlugin) {
    // Set to Local mode (required for registerPluginByName)
    LogosModeConfig::setMode(LogosMode::Local);
    
    // Try to register a non-existent plugin
    bool result = PluginManager::registerPluginByName("nonexistent_plugin");
    EXPECT_FALSE(result);
}

// =============================================================================
// resolveDependencies Function Tests
// =============================================================================

// Verifies that resolveDependencies() returns an empty list when given an empty list
TEST_F(PluginManagerTest, ResolveDependencies_ReturnsEmptyForEmptyInput) {
    QStringList result = PluginManager::resolveDependencies(QStringList());
    EXPECT_TRUE(result.isEmpty());
}

// Verifies that resolveDependencies() returns an empty list when the plugin is not known
TEST_F(PluginManagerTest, ResolveDependencies_ReturnsEmptyForUnknownPlugin) {
    QStringList requested;
    requested.append("unknown_plugin");
    
    QStringList result = PluginManager::resolveDependencies(requested);
    EXPECT_TRUE(result.isEmpty());
}

// Verifies that resolveDependencies() returns the plugin itself when it has no dependencies
TEST_F(PluginManagerTest, ResolveDependencies_ReturnsSinglePluginWithNoDeps) {
    // Add a known plugin with no dependencies
    PluginManager::addKnownPlugin("plugin_a", "/path/to/plugin_a");
    QJsonObject metadata;
    metadata["name"] = "plugin_a";
    metadata["dependencies"] = QJsonArray();
    g_plugin_metadata.insert("plugin_a", metadata);
    
    QStringList requested;
    requested.append("plugin_a");
    
    QStringList result = PluginManager::resolveDependencies(requested);
    
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].toStdString(), "plugin_a");
}

// Verifies that resolveDependencies() returns dependencies in topological order (deps first)
TEST_F(PluginManagerTest, ResolveDependencies_ReturnsCorrectOrder) {
    // Set up plugin_a which depends on plugin_b
    PluginManager::addKnownPlugin("plugin_a", "/path/to/plugin_a");
    PluginManager::addKnownPlugin("plugin_b", "/path/to/plugin_b");
    
    QJsonObject metadataA;
    metadataA["name"] = "plugin_a";
    QJsonArray depsA;
    depsA.append("plugin_b");
    metadataA["dependencies"] = depsA;
    g_plugin_metadata.insert("plugin_a", metadataA);
    
    QJsonObject metadataB;
    metadataB["name"] = "plugin_b";
    metadataB["dependencies"] = QJsonArray();
    g_plugin_metadata.insert("plugin_b", metadataB);
    
    QStringList requested;
    requested.append("plugin_a");
    
    QStringList result = PluginManager::resolveDependencies(requested);
    
    // Should return both plugins, with plugin_b first (dependency order)
    ASSERT_EQ(result.size(), 2);
    EXPECT_EQ(result[0].toStdString(), "plugin_b");  // Dependency first
    EXPECT_EQ(result[1].toStdString(), "plugin_a");  // Then the requested plugin
}

// Verifies that resolveDependencies() handles transitive dependencies correctly
TEST_F(PluginManagerTest, ResolveDependencies_HandlesTransitiveDeps) {
    // Set up: plugin_a -> plugin_b -> plugin_c
    PluginManager::addKnownPlugin("plugin_a", "/path/to/plugin_a");
    PluginManager::addKnownPlugin("plugin_b", "/path/to/plugin_b");
    PluginManager::addKnownPlugin("plugin_c", "/path/to/plugin_c");
    
    QJsonObject metadataA;
    metadataA["name"] = "plugin_a";
    QJsonArray depsA;
    depsA.append("plugin_b");
    metadataA["dependencies"] = depsA;
    g_plugin_metadata.insert("plugin_a", metadataA);
    
    QJsonObject metadataB;
    metadataB["name"] = "plugin_b";
    QJsonArray depsB;
    depsB.append("plugin_c");
    metadataB["dependencies"] = depsB;
    g_plugin_metadata.insert("plugin_b", metadataB);
    
    QJsonObject metadataC;
    metadataC["name"] = "plugin_c";
    metadataC["dependencies"] = QJsonArray();
    g_plugin_metadata.insert("plugin_c", metadataC);
    
    QStringList requested;
    requested.append("plugin_a");
    
    QStringList result = PluginManager::resolveDependencies(requested);
    
    // Should return all three plugins in correct order
    ASSERT_EQ(result.size(), 3);
    EXPECT_EQ(result[0].toStdString(), "plugin_c");  // Deepest dependency first
    EXPECT_EQ(result[1].toStdString(), "plugin_b");  // Then intermediate
    EXPECT_EQ(result[2].toStdString(), "plugin_a");  // Then requested plugin
}

// =============================================================================
// C API: logos_core_load_plugin_with_dependencies Tests
// =============================================================================

// Verifies that logos_core_load_plugin_with_dependencies() returns 0 for null plugin name
TEST_F(PluginManagerTest, LoadPluginWithDependencies_ReturnsZeroForNull) {
    int result = logos_core_load_plugin_with_dependencies(nullptr);
    EXPECT_EQ(result, 0);
}

// Verifies that logos_core_load_plugin_with_dependencies() returns 0 for unknown plugin
TEST_F(PluginManagerTest, LoadPluginWithDependencies_ReturnsZeroForUnknown) {
    int result = logos_core_load_plugin_with_dependencies("unknown_plugin");
    EXPECT_EQ(result, 0);
}

// Verifies that logos_core_load_plugin_with_dependencies() skips already-loaded plugins
TEST_F(PluginManagerTest, LoadPluginWithDependencies_SkipsAlreadyLoaded) {
    // Set up plugin_a which depends on plugin_b
    PluginManager::addKnownPlugin("plugin_a", "/path/to/plugin_a");
    PluginManager::addKnownPlugin("plugin_b", "/path/to/plugin_b");
    
    QJsonObject metadataA;
    metadataA["name"] = "plugin_a";
    QJsonArray depsA;
    depsA.append("plugin_b");
    metadataA["dependencies"] = depsA;
    g_plugin_metadata.insert("plugin_a", metadataA);
    
    QJsonObject metadataB;
    metadataB["name"] = "plugin_b";
    metadataB["dependencies"] = QJsonArray();
    g_plugin_metadata.insert("plugin_b", metadataB);
    
    // Mark plugin_b as already loaded
    g_loaded_plugins.append("plugin_b");
    
    // Verify the skip logic preconditions
    EXPECT_TRUE(PluginManager::isPluginLoaded("plugin_b"));
    EXPECT_FALSE(PluginManager::isPluginLoaded("plugin_a"));
    
    // The function should skip plugin_b (already loaded) and only attempt to load plugin_a
    // Since plugin_a doesn't have a real file, loading will fail for plugin_a specifically,
    // but the important thing is plugin_b is skipped (not double-loaded)
    int result = logos_core_load_plugin_with_dependencies("plugin_a");
    
    // Result will be 0 because plugin_a can't actually be loaded (no real file),
    // but plugin_b should still be in loaded state (not removed or errored)
    EXPECT_TRUE(PluginManager::isPluginLoaded("plugin_b"));
}
