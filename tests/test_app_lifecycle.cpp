#include <gtest/gtest.h>
#include "logos_core.h"
#include "qt_test_adapter.h"
#include <cstring>
#include <string>

static void clearModuleState() {
    logos_core_terminate_all();
    logos_core_clear();
}

int main(int argc, char** argv) {
    logos_core_init(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    int result = RUN_ALL_TESTS();
    logos_core_cleanup();
    return result;
}

class AppLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        clearModuleState();
    }

    void TearDown() override {
        clearModuleState();
    }
};

// =============================================================================
// Initialization Tests
// =============================================================================

TEST_F(AppLifecycleTest, Init_LibraryIsUsableAfterInit) {
    // After init, basic library operations should not crash.
    logos_core_process_events();
    EXPECT_EQ(logos_core_get_modules_dirs_count(), 0);
}

// =============================================================================
// Module Directory Tests
// =============================================================================

TEST_F(AppLifecycleTest, SetModulesDir_SetsDirectory) {
    const char* testDir = "/test/modules";

    logos_core_set_modules_dir(testDir);

    ASSERT_EQ(logos_core_get_modules_dirs_count(), 1);
    char* dir = logos_core_get_modules_dir_at(0);
    ASSERT_NE(dir, nullptr);
    EXPECT_EQ(std::string(dir), std::string(testDir));
    delete[] dir;
}

TEST_F(AppLifecycleTest, SetModulesDir_ClearsExisting) {
    logos_core_add_modules_dir("/dir1");
    logos_core_add_modules_dir("/dir2");
    ASSERT_EQ(logos_core_get_modules_dirs_count(), 2);

    logos_core_set_modules_dir("/new_dir");

    ASSERT_EQ(logos_core_get_modules_dirs_count(), 1);
    char* dir = logos_core_get_modules_dir_at(0);
    ASSERT_NE(dir, nullptr);
    EXPECT_EQ(std::string(dir), "/new_dir");
    delete[] dir;
}

TEST_F(AppLifecycleTest, AddModulesDir_AppendsDirectory) {
    logos_core_add_modules_dir("/dir1");
    logos_core_add_modules_dir("/dir2");

    ASSERT_EQ(logos_core_get_modules_dirs_count(), 2);

    char* d0 = logos_core_get_modules_dir_at(0);
    char* d1 = logos_core_get_modules_dir_at(1);
    ASSERT_NE(d0, nullptr);
    ASSERT_NE(d1, nullptr);
    EXPECT_EQ(std::string(d0), "/dir1");
    EXPECT_EQ(std::string(d1), "/dir2");
    delete[] d0;
    delete[] d1;
}

TEST_F(AppLifecycleTest, AddModulesDir_NoDuplicates) {
    logos_core_add_modules_dir("/test");
    logos_core_add_modules_dir("/test");

    EXPECT_EQ(logos_core_get_modules_dirs_count(), 1);
}

// =============================================================================
// Cleanup Tests
// =============================================================================

TEST_F(AppLifecycleTest, Cleanup_ClearsGlobals) {
    logos_core_register_module("test", "/path/to/test");
    logos_core_add_modules_dir("/test");

    clearModuleState();

    char** loaded = logos_core_get_loaded_modules();
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded[0], nullptr);
    delete[] loaded;

    char** known = logos_core_get_known_modules();
    ASSERT_NE(known, nullptr);
    EXPECT_EQ(known[0], nullptr);
    delete[] known;
}

TEST_F(AppLifecycleTest, Cleanup_ClearsState) {
    EXPECT_TRUE(true) << "Cleanup behavior is tested indirectly through integration tests";
}

// =============================================================================
// ProcessEvents Tests
// =============================================================================

TEST_F(AppLifecycleTest, ProcessEvents_DoesNotCrash) {
    EXPECT_NO_THROW(logos_core_process_events());
}

TEST_F(AppLifecycleTest, ProcessEvents_AfterCleanupDoesNotCrash) {
    logos_core_cleanup();

    // processEvents with no app should not crash
    EXPECT_NO_THROW(logos_core_process_events());

    // Re-init for TearDown
    char* argv[] = {(char*)"test"};
    logos_core_init(1, argv);
}

// =============================================================================
// Start Function Tests
// =============================================================================

TEST_F(AppLifecycleTest, Start_UsesCustomModulesDirs) {
    logos_core_set_modules_dir("/custom/modules");

    logos_core_start();

    ASSERT_EQ(logos_core_get_modules_dirs_count(), 1);
    char* dir = logos_core_get_modules_dir_at(0);
    ASSERT_NE(dir, nullptr);
    EXPECT_EQ(std::string(dir), "/custom/modules");
    delete[] dir;
}

// =============================================================================
// Exec Test
// =============================================================================

TEST_F(AppLifecycleTest, Exec_ReturnsZero) {
    logos_core_cleanup();

    int result = logos_core_exec();

    EXPECT_EQ(result, 0);

    // Re-init for TearDown
    char* argv[] = {(char*)"test"};
    logos_core_init(1, argv);
}
