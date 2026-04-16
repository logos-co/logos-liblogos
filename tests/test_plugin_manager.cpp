#include <gtest/gtest.h>
#include "logos_core.h"
#include "qt_test_adapter.h"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void clearPluginState() {
    logos_core_terminate_all();
    logos_core_clear();
}

// RAII temporary directory (uses mkdtemp, cleaned up on destruction)
struct TmpDir {
    fs::path path;

    TmpDir() {
        std::string tmpl = (fs::temp_directory_path() / "logos_test_XXXXXX").string();
        char* buf = new char[tmpl.size() + 1];
        memcpy(buf, tmpl.c_str(), tmpl.size() + 1);
        if (!mkdtemp(buf)) {
            delete[] buf;
            throw std::runtime_error("mkdtemp failed");
        }
        path = buf;
        delete[] buf;
    }

    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    bool isValid() const { return fs::is_directory(path); }

    // Returns path.string().c_str()-compatible value as std::string
    std::string str() const { return path.string(); }
};

static void createFakeModule(const fs::path& parentDir,
                             const std::string& moduleName,
                             const std::string& mainFile,
                             const std::string& type = "core",
                             const std::vector<std::string>& dependencies = {}) {
    fs::path moduleDir = parentDir / moduleName;
    fs::create_directories(moduleDir);

    nlohmann::json manifest;
    manifest["name"] = moduleName;
    manifest["version"] = "1.0.0";
    manifest["type"] = type;
    manifest["main"] = mainFile;
    manifest["description"] = "Fake test module";
    if (!dependencies.empty())
        manifest["dependencies"] = dependencies;

    std::ofstream mf(moduleDir / "manifest.json");
    mf << manifest.dump();
    mf.close();

    std::ofstream bf(moduleDir / mainFile);
    bf << "fake";
    bf.close();
}

// Helpers to free null-terminated char** arrays returned by the C API.
static void freeStringArray(char** arr) {
    if (!arr) return;
    for (int i = 0; arr[i] != nullptr; ++i)
        delete[] arr[i];
    delete[] arr;
}

static int stringArrayLen(char** arr) {
    if (!arr) return 0;
    int n = 0;
    while (arr[n]) ++n;
    return n;
}

static std::set<std::string> stringArrayToSet(char** arr) {
    std::set<std::string> s;
    if (!arr) return s;
    for (int i = 0; arr[i]; ++i)
        s.insert(arr[i]);
    return s;
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
    char** result = logos_core_get_loaded_plugins();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);
    delete[] result;
}

TEST_F(PluginManagerTest, GetKnownPlugins_ReturnsEmptyHash) {
    char** result = logos_core_get_known_plugins();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);
    delete[] result;
}

TEST_F(PluginManagerTest, GetKnownPlugins_ReturnsCorrectHash) {
    logos_core_register_plugin("plugin1", "/path/to/plugin1.dylib");
    logos_core_register_plugin("plugin2", "/path/to/plugin2.dylib");

    char** result = logos_core_get_known_plugins();
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(stringArrayLen(result), 2);

    auto pluginSet = stringArrayToSet(result);
    EXPECT_TRUE(pluginSet.count("plugin1"));
    EXPECT_TRUE(pluginSet.count("plugin2"));

    char* path1 = logos_core_get_plugin_path("plugin1");
    char* path2 = logos_core_get_plugin_path("plugin2");
    ASSERT_NE(path1, nullptr);
    ASSERT_NE(path2, nullptr);
    EXPECT_EQ(std::string(path1), "/path/to/plugin1.dylib");
    EXPECT_EQ(std::string(path2), "/path/to/plugin2.dylib");
    delete[] path1;
    delete[] path2;
    freeStringArray(result);
}

TEST_F(PluginManagerTest, IsPluginLoaded_ReturnsFalseForUnloaded) {
    EXPECT_EQ(logos_core_is_plugin_loaded("nonexistent_plugin"), 0);
}

TEST_F(PluginManagerTest, IsPluginKnown_ReturnsFalseForUnknown) {
    EXPECT_EQ(logos_core_is_plugin_known("nonexistent_plugin"), 0);
}

TEST_F(PluginManagerTest, IsPluginKnown_ReturnsTrueForKnown) {
    logos_core_register_plugin("test_plugin", "/path/to/plugin");

    EXPECT_EQ(logos_core_is_plugin_known("test_plugin"), 1);
}

// =============================================================================
// C String Array Functions Tests
// =============================================================================

TEST_F(PluginManagerTest, GetLoadedPluginsCStr_ReturnsNullTerminatedArrayWhenEmpty) {
    char** result = logos_core_get_loaded_plugins();

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);

    delete[] result;
}

TEST_F(PluginManagerTest, GetKnownPluginsCStr_ReturnsNullTerminatedArrayWhenEmpty) {
    char** result = logos_core_get_known_plugins();

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);

    delete[] result;
}

TEST_F(PluginManagerTest, GetKnownPluginsCStr_ReturnsCorrectArray) {
    logos_core_register_plugin("plugin1", "/path/to/plugin1");
    logos_core_register_plugin("plugin2", "/path/to/plugin2");

    char** result = logos_core_get_known_plugins();

    ASSERT_NE(result, nullptr);
    ASSERT_NE(result[0], nullptr);
    ASSERT_NE(result[1], nullptr);
    EXPECT_EQ(result[2], nullptr);

    auto plugins = stringArrayToSet(result);
    EXPECT_TRUE(plugins.count("plugin1"));
    EXPECT_TRUE(plugins.count("plugin2"));

    freeStringArray(result);
}

// =============================================================================
// loadPlugin Error Cases Tests
// =============================================================================

TEST_F(PluginManagerTest, LoadPlugin_ReturnsFalseForUnknownPlugin) {
    int result = logos_core_load_plugin("nonexistent_plugin");
    EXPECT_EQ(result, 0);
}

// =============================================================================
// unloadPlugin Error Cases Tests
// =============================================================================

TEST_F(PluginManagerTest, UnloadPlugin_ReturnsFalseForNotLoaded) {
    int result = logos_core_unload_plugin("nonexistent_plugin");
    EXPECT_EQ(result, 0);
}

// =============================================================================
// resolveDependencies Function Tests
// =============================================================================

TEST_F(PluginManagerTest, ResolveDependencies_ReturnsEmptyForEmptyInput) {
    char** result = logos_core_resolve_dependencies(nullptr, 0);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);
    delete[] result;
}

TEST_F(PluginManagerTest, ResolveDependencies_ReturnsEmptyForUnknownPlugin) {
    const char* names[] = {"unknown_plugin"};
    char** result = logos_core_resolve_dependencies(names, 1);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);
    delete[] result;
}

TEST_F(PluginManagerTest, ResolveDependencies_ReturnsSinglePluginWithNoDeps) {
    logos_core_register_plugin("plugin_a", "/path/to/plugin_a");
    logos_core_register_plugin_dependencies("plugin_a", nullptr, 0);

    const char* names[] = {"plugin_a"};
    char** result = logos_core_resolve_dependencies(names, 1);

    ASSERT_EQ(stringArrayLen(result), 1);
    EXPECT_EQ(std::string(result[0]), "plugin_a");

    freeStringArray(result);
}

TEST_F(PluginManagerTest, ResolveDependencies_ReturnsCorrectOrder) {
    logos_core_register_plugin("plugin_a", "/path/to/plugin_a");
    logos_core_register_plugin("plugin_b", "/path/to/plugin_b");
    const char* depsA[] = {"plugin_b"};
    logos_core_register_plugin_dependencies("plugin_a", depsA, 1);
    logos_core_register_plugin_dependencies("plugin_b", nullptr, 0);

    const char* names[] = {"plugin_a"};
    char** result = logos_core_resolve_dependencies(names, 1);

    ASSERT_EQ(stringArrayLen(result), 2);
    EXPECT_EQ(std::string(result[0]), "plugin_b");
    EXPECT_EQ(std::string(result[1]), "plugin_a");

    freeStringArray(result);
}

TEST_F(PluginManagerTest, ResolveDependencies_HandlesTransitiveDeps) {
    logos_core_register_plugin("plugin_a", "/path/to/plugin_a");
    logos_core_register_plugin("plugin_b", "/path/to/plugin_b");
    logos_core_register_plugin("plugin_c", "/path/to/plugin_c");
    const char* depsA[] = {"plugin_b"};
    const char* depsB[] = {"plugin_c"};
    logos_core_register_plugin_dependencies("plugin_a", depsA, 1);
    logos_core_register_plugin_dependencies("plugin_b", depsB, 1);
    logos_core_register_plugin_dependencies("plugin_c", nullptr, 0);

    const char* names[] = {"plugin_a"};
    char** result = logos_core_resolve_dependencies(names, 1);

    ASSERT_EQ(stringArrayLen(result), 3);
    EXPECT_EQ(std::string(result[0]), "plugin_c");
    EXPECT_EQ(std::string(result[1]), "plugin_b");
    EXPECT_EQ(std::string(result[2]), "plugin_a");

    freeStringArray(result);
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
    logos_core_set_plugins_dir("/tmp/test_plugins");
    ASSERT_EQ(logos_core_get_plugins_dirs_count(), 1);
    char* dir = logos_core_get_plugins_dir_at(0);
    ASSERT_NE(dir, nullptr);
    EXPECT_EQ(std::string(dir), "/tmp/test_plugins");
    delete[] dir;
}

TEST_F(PluginManagerTest, AddPluginsDir_AppendsDirectory) {
    logos_core_set_plugins_dir("/tmp/dir1");
    logos_core_add_plugins_dir("/tmp/dir2");
    logos_core_add_plugins_dir("/tmp/dir3");

    ASSERT_EQ(logos_core_get_plugins_dirs_count(), 3);

    char* d0 = logos_core_get_plugins_dir_at(0);
    char* d1 = logos_core_get_plugins_dir_at(1);
    char* d2 = logos_core_get_plugins_dir_at(2);
    ASSERT_NE(d0, nullptr);
    ASSERT_NE(d1, nullptr);
    ASSERT_NE(d2, nullptr);
    EXPECT_EQ(std::string(d0), "/tmp/dir1");
    EXPECT_EQ(std::string(d1), "/tmp/dir2");
    EXPECT_EQ(std::string(d2), "/tmp/dir3");
    delete[] d0;
    delete[] d1;
    delete[] d2;
}

TEST_F(PluginManagerTest, GetPluginsDirs_ReturnsEmptyAfterClear) {
    logos_core_set_plugins_dir("/tmp/dir1");
    clearPluginState();
    EXPECT_EQ(logos_core_get_plugins_dirs_count(), 0);
}

// =============================================================================
// Discovery Tests — fake installed modules
// =============================================================================

TEST_F(PluginManagerTest, DiscoverInstalledModules_DoesNotCrashWithEmptyDir) {
    TmpDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    logos_core_set_plugins_dir(tmpDir.str().c_str());
    logos_core_refresh_plugins();

    char** known = logos_core_get_known_plugins();
    ASSERT_NE(known, nullptr);
    EXPECT_EQ(known[0], nullptr);
    delete[] known;
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_DoesNotCrashWithNonexistentDir) {
    logos_core_set_plugins_dir("/tmp/nonexistent_dir_12345");
    logos_core_refresh_plugins();

    char** known = logos_core_get_known_plugins();
    ASSERT_NE(known, nullptr);
    EXPECT_EQ(known[0], nullptr);
    delete[] known;
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_FindsFakeModulesWithoutCrash) {
    TmpDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    createFakeModule(tmpDir.path, "fake_module_a", "fake_module_a_plugin.so");
    createFakeModule(tmpDir.path, "fake_module_b", "fake_module_b_plugin.so");

    logos_core_set_plugins_dir(tmpDir.str().c_str());
    logos_core_refresh_plugins();

    char** known = logos_core_get_known_plugins();
    ASSERT_NE(known, nullptr);
    EXPECT_EQ(known[0], nullptr);
    delete[] known;
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_IgnoresModulesWithoutManifest) {
    TmpDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    fs::path moduleDir = tmpDir.path / "no_manifest_module";
    fs::create_directories(moduleDir);
    std::ofstream bf(moduleDir / "plugin.so");
    bf << "fake";
    bf.close();

    logos_core_set_plugins_dir(tmpDir.str().c_str());
    logos_core_refresh_plugins();

    char** known = logos_core_get_known_plugins();
    ASSERT_NE(known, nullptr);
    EXPECT_EQ(known[0], nullptr);
    delete[] known;
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_IgnoresUiTypeModules) {
    TmpDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    createFakeModule(tmpDir.path, "ui_module", "ui_module_plugin.so", "ui");

    logos_core_set_plugins_dir(tmpDir.str().c_str());
    logos_core_refresh_plugins();

    char** known = logos_core_get_known_plugins();
    ASSERT_NE(known, nullptr);
    EXPECT_EQ(known[0], nullptr);
    delete[] known;
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_MultipleDirectories) {
    TmpDir tmpDir1;
    TmpDir tmpDir2;
    ASSERT_TRUE(tmpDir1.isValid());
    ASSERT_TRUE(tmpDir2.isValid());

    createFakeModule(tmpDir1.path, "module_in_dir1", "module_in_dir1_plugin.so");
    createFakeModule(tmpDir2.path, "module_in_dir2", "module_in_dir2_plugin.so");

    logos_core_set_plugins_dir(tmpDir1.str().c_str());
    logos_core_add_plugins_dir(tmpDir2.str().c_str());

    ASSERT_EQ(logos_core_get_plugins_dirs_count(), 2);

    logos_core_refresh_plugins();

    char** known = logos_core_get_known_plugins();
    ASSERT_NE(known, nullptr);
    EXPECT_EQ(known[0], nullptr);
    delete[] known;
}

TEST_F(PluginManagerTest, DiscoverInstalledModules_InvalidManifestJson) {
    TmpDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    fs::path moduleDir = tmpDir.path / "bad_manifest_module";
    fs::create_directories(moduleDir);
    std::ofstream mf(moduleDir / "manifest.json");
    mf << "{ this is not valid json }}}";
    mf.close();

    logos_core_set_plugins_dir(tmpDir.str().c_str());
    logos_core_refresh_plugins();

    char** known = logos_core_get_known_plugins();
    ASSERT_NE(known, nullptr);
    EXPECT_EQ(known[0], nullptr);
    delete[] known;
}

// =============================================================================
// Loaded-flag preservation across re-registration
// =============================================================================

TEST_F(PluginManagerTest, RegisterPlugin_PreservesLoadedFlagOnReregister) {
    logos_core_register_plugin("test_plugin", "/path/v1");
    logos_core_mark_plugin_loaded("test_plugin");
    ASSERT_EQ(logos_core_is_plugin_loaded("test_plugin"), 1);

    logos_core_register_plugin("test_plugin", "/path/v2");

    EXPECT_EQ(logos_core_is_plugin_loaded("test_plugin"), 1)
        << "Re-registering a known plugin must preserve its loaded flag";

    char* path = logos_core_get_plugin_path("test_plugin");
    ASSERT_NE(path, nullptr);
    EXPECT_EQ(std::string(path), "/path/v2");
    delete[] path;
}

TEST_F(PluginManagerTest, RegisterDependencies_PreservesLoadedFlag) {
    logos_core_register_plugin("test_plugin", "/path/to/plugin");
    logos_core_mark_plugin_loaded("test_plugin");
    ASSERT_EQ(logos_core_is_plugin_loaded("test_plugin"), 1);

    const char* deps[] = {"dep_a", "dep_b"};
    logos_core_register_plugin_dependencies("test_plugin", deps, 2);

    EXPECT_EQ(logos_core_is_plugin_loaded("test_plugin"), 1)
        << "Updating dependencies must not wipe the loaded flag";
    EXPECT_EQ(logos_core_get_plugin_dependencies_count("test_plugin"), 2);
}

// =============================================================================
// End-to-end regression tests using a real Qt plugin.
// =============================================================================

class RealPluginRegistryTest : public ::testing::Test {
protected:
    std::string pluginPath;

    void SetUp() override {
        clearPluginState();

        const char* envPlugin = std::getenv("TEST_PLUGIN");
        if (envPlugin && std::strlen(envPlugin) > 0 &&
            fs::exists(envPlugin)) {
            pluginPath = envPlugin;
            return;
        }

        GTEST_SKIP() << "No real test plugin available. "
                     << "Set TEST_PLUGIN env var to a built Qt plugin (.so/.dylib).";
    }

    void TearDown() override {
        clearPluginState();
    }
};

TEST_F(RealPluginRegistryTest, ProcessPlugin_RegistersRealPlugin) {
    char* name = logos_core_process_plugin(pluginPath.c_str());
    ASSERT_NE(name, nullptr) << "process_plugin failed for " << pluginPath;
    EXPECT_NE(std::string(name), "");
    EXPECT_EQ(logos_core_is_plugin_known(name), 1);
    EXPECT_EQ(logos_core_is_plugin_loaded(name), 0);
    delete[] name;
}

// =============================================================================
// Cascading unload: logos_core_unload_plugin_with_dependents
//
// The cascade is exercised without real Qt plugins. We:
//   1. Set up fake manifests on disk (PackageManagerLib scan sees the
//      dependency edges).
//   2. Register the same plugins directly in PluginRegistry so it believes
//      they exist (the fake .so files are not loadable Qt plugins, so
//      refresh_plugins alone wouldn't populate the registry).
//   3. Register placeholder "processes" + mark loaded so hasProcess() returns
//      true — `terminateProcess` on a placeholder is a no-op but still
//      removes the entry cleanly.
// =============================================================================

class CascadeUnloadTest : public ::testing::Test {
protected:
    TmpDir tmpDir;

    void SetUp() override {
        clearPluginState();
        logos_core_clear_processes();
    }

    void TearDown() override {
        clearPluginState();
        logos_core_clear_processes();
    }

    // Registers a plugin in both PluginRegistry and as a loaded fake process.
    void setupLoaded(const std::string& name,
                     const std::vector<std::string>& deps = {}) {
        std::string path = (tmpDir.path / name / (name + "_plugin.so")).string();
        logos_core_register_plugin(name.c_str(), path.c_str());
        std::vector<const char*> depPtrs;
        depPtrs.reserve(deps.size());
        for (const auto& d : deps) depPtrs.push_back(d.c_str());
        logos_core_register_plugin_dependencies(
            name.c_str(),
            depPtrs.empty() ? nullptr : depPtrs.data(),
            static_cast<int>(depPtrs.size()));

        logos_core_register_process(name.c_str());
        logos_core_mark_plugin_loaded(name.c_str());
    }

    void writeManifestsAndScan(
        const std::vector<std::tuple<std::string, std::vector<std::string>>>& modules)
    {
        for (const auto& [name, deps] : modules) {
            createFakeModule(tmpDir.path, name, name + "_plugin.so", "core", deps);
        }
        logos_core_set_plugins_dir(tmpDir.str().c_str());
        logos_core_refresh_plugins();
    }
};

TEST_F(CascadeUnloadTest, UnloadPluginWithDependents_ReturnsZeroWhenTargetNotLoaded) {
    // Plugin is known but not loaded.
    logos_core_register_plugin("foo", "/foo");
    int result = logos_core_unload_plugin_with_dependents("foo");
    EXPECT_EQ(result, 0);
}

TEST_F(CascadeUnloadTest, UnloadPluginWithDependents_NoDependents_UnloadsTargetOnly) {
    // Single loaded plugin with no dependents on disk → cascade is just the
    // target.
    writeManifestsAndScan({ {"solo", {}} });
    setupLoaded("solo");

    ASSERT_EQ(logos_core_is_plugin_loaded("solo"), 1);

    int result = logos_core_unload_plugin_with_dependents("solo");
    EXPECT_EQ(result, 1);
    EXPECT_EQ(logos_core_is_plugin_loaded("solo"), 0);
    EXPECT_EQ(logos_core_has_process("solo"), 0);
}

TEST_F(CascadeUnloadTest, UnloadPluginWithDependents_RecursiveDependentsLeavesFirst) {
    // Graph: a -> b -> c (a depends on b, b depends on c).
    // Unloading c should also bring down b and a, in the order a, b, c.
    writeManifestsAndScan({
        {"c", {}},
        {"b", {"c"}},
        {"a", {"b"}},
    });
    setupLoaded("c", {});
    setupLoaded("b", {"c"});
    setupLoaded("a", {"b"});

    ASSERT_EQ(logos_core_is_plugin_loaded("a"), 1);
    ASSERT_EQ(logos_core_is_plugin_loaded("b"), 1);
    ASSERT_EQ(logos_core_is_plugin_loaded("c"), 1);

    int result = logos_core_unload_plugin_with_dependents("c");
    EXPECT_EQ(result, 1);

    EXPECT_EQ(logos_core_is_plugin_loaded("a"), 0);
    EXPECT_EQ(logos_core_is_plugin_loaded("b"), 0);
    EXPECT_EQ(logos_core_is_plugin_loaded("c"), 0);

    EXPECT_EQ(logos_core_has_process("a"), 0);
    EXPECT_EQ(logos_core_has_process("b"), 0);
    EXPECT_EQ(logos_core_has_process("c"), 0);
}

TEST_F(CascadeUnloadTest, UnloadPluginWithDependents_UnloadedDependentsIgnored) {
    // b depends on c. Only c is loaded; b is known but not loaded. Cascade
    // should only touch c. b stays unloaded (not "failed to unload").
    writeManifestsAndScan({
        {"c", {}},
        {"b", {"c"}},
    });
    setupLoaded("c", {});
    // Register b in registry but don't mark it loaded.
    logos_core_register_plugin("b", "/b");
    const char* depsB[] = {"c"};
    logos_core_register_plugin_dependencies("b", depsB, 1);

    int result = logos_core_unload_plugin_with_dependents("c");
    EXPECT_EQ(result, 1);

    EXPECT_EQ(logos_core_is_plugin_loaded("c"), 0);
    EXPECT_EQ(logos_core_is_plugin_loaded("b"), 0);
}

TEST_F(CascadeUnloadTest, UnloadPluginWithDependents_DiamondDependents) {
    // Diamond: a -> b -> d ; a -> c -> d. Unloading d should bring down
    // a, b, c in some valid order (a before b and c; b and c before d).
    writeManifestsAndScan({
        {"d", {}},
        {"b", {"d"}},
        {"c", {"d"}},
        {"a", {"b", "c"}},
    });
    setupLoaded("d", {});
    setupLoaded("b", {"d"});
    setupLoaded("c", {"d"});
    setupLoaded("a", {"b", "c"});

    int result = logos_core_unload_plugin_with_dependents("d");
    EXPECT_EQ(result, 1);

    EXPECT_EQ(logos_core_is_plugin_loaded("a"), 0);
    EXPECT_EQ(logos_core_is_plugin_loaded("b"), 0);
    EXPECT_EQ(logos_core_is_plugin_loaded("c"), 0);
    EXPECT_EQ(logos_core_is_plugin_loaded("d"), 0);
}

TEST_F(CascadeUnloadTest, UnloadPluginWithDependents_AbortsForNull) {
    EXPECT_DEATH(logos_core_unload_plugin_with_dependents(nullptr), "");
}

TEST_F(RealPluginRegistryTest, ProcessPlugin_PreservesLoadedFlagOnReprocess) {
    char* name1 = logos_core_process_plugin(pluginPath.c_str());
    ASSERT_NE(name1, nullptr) << "process_plugin failed for " << pluginPath;
    std::string pluginName = name1;
    delete[] name1;

    ASSERT_EQ(logos_core_is_plugin_known(pluginName.c_str()), 1);

    logos_core_mark_plugin_loaded(pluginName.c_str());
    ASSERT_EQ(logos_core_is_plugin_loaded(pluginName.c_str()), 1);

    char* name2 = logos_core_process_plugin(pluginPath.c_str());
    ASSERT_NE(name2, nullptr);
    EXPECT_EQ(std::string(name2), pluginName);
    delete[] name2;

    EXPECT_EQ(logos_core_is_plugin_loaded(pluginName.c_str()), 1)
        << "Re-processing a loaded plugin must preserve its loaded flag";

    // Verify it still appears in the loaded list
    char** loaded = logos_core_get_loaded_plugins();
    auto loadedSet = stringArrayToSet(loaded);
    freeStringArray(loaded);
    EXPECT_TRUE(loadedSet.count(pluginName))
        << "get_loaded_plugins() must still report the plugin as loaded";
}

// =============================================================================
// Dependency graph queries:
//   logos_core_get_module_dependencies(name, recursive)
//   logos_core_get_module_dependents(name, recursive)
//
// These read from the in-process registry. We populate it with
// logos_core_register_plugin + logos_core_register_plugin_dependencies
// (which in turn trigger recomputeDependentsLocked), then check both the
// direct and recursive traversals against known-shaped graphs.
// =============================================================================

class DependencyQueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        clearPluginState();
    }

    void TearDown() override {
        clearPluginState();
    }

    // Register a plugin with a (possibly empty) direct dependency list. Path
    // value isn't exercised by the queries — anything non-empty is fine.
    void reg(const std::string& name,
             const std::vector<std::string>& deps = {}) {
        logos_core_register_plugin(name.c_str(), ("/" + name).c_str());
        std::vector<const char*> depPtrs;
        depPtrs.reserve(deps.size());
        for (const auto& d : deps) depPtrs.push_back(d.c_str());
        logos_core_register_plugin_dependencies(
            name.c_str(),
            depPtrs.empty() ? nullptr : depPtrs.data(),
            static_cast<int>(depPtrs.size()));
    }
};

TEST_F(DependencyQueryTest, GetModuleDependencies_UnknownName_ReturnsEmpty) {
    char** deps = logos_core_get_module_dependencies("ghost", false);
    EXPECT_EQ(stringArrayLen(deps), 0);
    freeStringArray(deps);

    deps = logos_core_get_module_dependencies("ghost", true);
    EXPECT_EQ(stringArrayLen(deps), 0);
    freeStringArray(deps);
}

TEST_F(DependencyQueryTest, GetModuleDependents_UnknownName_ReturnsEmpty) {
    char** d = logos_core_get_module_dependents("ghost", false);
    EXPECT_EQ(stringArrayLen(d), 0);
    freeStringArray(d);

    d = logos_core_get_module_dependents("ghost", true);
    EXPECT_EQ(stringArrayLen(d), 0);
    freeStringArray(d);
}

TEST_F(DependencyQueryTest, GetModuleDependencies_NoDeps_ReturnsEmpty) {
    reg("leaf");
    char** deps = logos_core_get_module_dependencies("leaf", false);
    EXPECT_EQ(stringArrayLen(deps), 0);
    freeStringArray(deps);
    deps = logos_core_get_module_dependencies("leaf", true);
    EXPECT_EQ(stringArrayLen(deps), 0);
    freeStringArray(deps);
}

TEST_F(DependencyQueryTest, GetModuleDependencies_DirectVsRecursive) {
    // Chain: a -> b -> c. Direct deps of a = {b}. Recursive deps of a = {b, c}.
    reg("c");
    reg("b", {"c"});
    reg("a", {"b"});

    char** direct = logos_core_get_module_dependencies("a", false);
    EXPECT_EQ(stringArrayToSet(direct), (std::set<std::string>{"b"}));
    freeStringArray(direct);

    char** recursive = logos_core_get_module_dependencies("a", true);
    EXPECT_EQ(stringArrayToSet(recursive), (std::set<std::string>{"b", "c"}));
    freeStringArray(recursive);
}

TEST_F(DependencyQueryTest, GetModuleDependents_DirectVsRecursive) {
    // Chain: a -> b -> c. Direct dependents of c = {b}. Recursive = {b, a}.
    reg("c");
    reg("b", {"c"});
    reg("a", {"b"});

    char** direct = logos_core_get_module_dependents("c", false);
    EXPECT_EQ(stringArrayToSet(direct), (std::set<std::string>{"b"}));
    freeStringArray(direct);

    char** recursive = logos_core_get_module_dependents("c", true);
    EXPECT_EQ(stringArrayToSet(recursive), (std::set<std::string>{"b", "a"}));
    freeStringArray(recursive);
}

TEST_F(DependencyQueryTest, GetModuleDependencies_Diamond_RecursiveDeduplicates) {
    // Diamond: a -> b -> d ; a -> c -> d. Recursive deps of a must include
    // {b, c, d} with no duplicate entries for d.
    reg("d");
    reg("b", {"d"});
    reg("c", {"d"});
    reg("a", {"b", "c"});

    char** recursive = logos_core_get_module_dependencies("a", true);
    std::set<std::string> got = stringArrayToSet(recursive);
    int n = stringArrayLen(recursive);
    freeStringArray(recursive);

    EXPECT_EQ(got, (std::set<std::string>{"b", "c", "d"}));
    // No duplicate d entries — set and array length must agree.
    EXPECT_EQ(n, static_cast<int>(got.size()));
}

TEST_F(DependencyQueryTest, GetModuleDependents_Diamond_RecursiveDeduplicates) {
    // Same diamond — d has {b, c} as direct dependents and {b, c, a}
    // transitively. The BFS must not report a twice even though both
    // b and c list it as a dependent.
    reg("d");
    reg("b", {"d"});
    reg("c", {"d"});
    reg("a", {"b", "c"});

    char** direct = logos_core_get_module_dependents("d", false);
    EXPECT_EQ(stringArrayToSet(direct), (std::set<std::string>{"b", "c"}));
    freeStringArray(direct);

    char** recursive = logos_core_get_module_dependents("d", true);
    std::set<std::string> got = stringArrayToSet(recursive);
    int n = stringArrayLen(recursive);
    freeStringArray(recursive);
    EXPECT_EQ(got, (std::set<std::string>{"a", "b", "c"}));
    EXPECT_EQ(n, static_cast<int>(got.size()));
}

TEST_F(DependencyQueryTest, GetModuleDependencies_SelfNotIncluded) {
    reg("leaf");
    reg("root", {"leaf"});
    char** recursive = logos_core_get_module_dependencies("root", true);
    std::set<std::string> got = stringArrayToSet(recursive);
    freeStringArray(recursive);
    EXPECT_EQ(got.count("root"), 0u);
    EXPECT_EQ(got, (std::set<std::string>{"leaf"}));
}

TEST_F(DependencyQueryTest, GetModuleDependencies_AbortsForNull) {
    EXPECT_DEATH(logos_core_get_module_dependencies(nullptr, false), "");
}

TEST_F(DependencyQueryTest, GetModuleDependents_AbortsForNull) {
    EXPECT_DEATH(logos_core_get_module_dependents(nullptr, false), "");
}
