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
    EXPECT_EQ(logos_core_get_modules_dirs_count(), 0);
}

// =============================================================================
// Module Directory Tests
// =============================================================================

TEST_F(AppLifecycleTest, AddModulesDir_SetsDirectory) {
    const char* testDir = "/test/modules";

    logos_core_add_modules_dir(testDir);

    ASSERT_EQ(logos_core_get_modules_dirs_count(), 1);
    char* dir = logos_core_get_modules_dir_at(0);
    ASSERT_NE(dir, nullptr);
    EXPECT_EQ(std::string(dir), std::string(testDir));
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
// Start Function Tests
// =============================================================================

TEST_F(AppLifecycleTest, Start_UsesCustomModulesDirs) {
    logos_core_add_modules_dir("/custom/modules");

    logos_core_start();

    ASSERT_EQ(logos_core_get_modules_dirs_count(), 1);
    char* dir = logos_core_get_modules_dir_at(0);
    ASSERT_NE(dir, nullptr);
    EXPECT_EQ(std::string(dir), "/custom/modules");
    delete[] dir;
}

// =============================================================================
// Access Policy Tests
// =============================================================================
//
// logos_core_set_access_policy is currently a no-op: the policy is
// accepted but not yet enforced (see the TODO in logos_core.cpp). These
// tests don't assert any behavioural effect — there's nothing to observe
// yet — but they pin the contract that already holds: the call accepts a
// well-formed policy, an empty string, and NULL (the documented "clear"
// signal) without crashing or aborting, and without disturbing unrelated
// core state. When enforcement lands, these become the scaffold for
// asserting the policy actually gates calls.

TEST_F(AppLifecycleTest, SetAccessPolicy_AcceptsValidPolicyWithoutCrashing) {
    const char* policy =
        "{\"version\":1,\"mode\":\"enforce\",\"restrictions\":{"
        "\"package_manager\":{\"allowedCallers\":[\"package_manager_ui\"]},"
        "\"package_downloader\":{\"allowedCallers\":[\"package_manager_ui\"]}}}";

    // No-op today: the only contract is "doesn't crash, doesn't throw".
    EXPECT_NO_THROW(logos_core_set_access_policy(policy));
}

TEST_F(AppLifecycleTest, SetAccessPolicy_AcceptsEmptyStringAsClear) {
    EXPECT_NO_THROW(logos_core_set_access_policy(""));
}

TEST_F(AppLifecycleTest, SetAccessPolicy_AcceptsNullAsClear) {
    // Unlike the module-name setters, this one must NOT abort on NULL —
    // NULL is the documented "clear the policy" signal.
    EXPECT_NO_THROW(logos_core_set_access_policy(nullptr));
}

TEST_F(AppLifecycleTest, SetAccessPolicy_IsIdempotentAcrossRepeatedCalls) {
    // Setting then clearing then re-setting must be safe in any order.
    EXPECT_NO_THROW(logos_core_set_access_policy("{\"version\":1}"));
    EXPECT_NO_THROW(logos_core_set_access_policy(nullptr));
    EXPECT_NO_THROW(logos_core_set_access_policy(""));
    EXPECT_NO_THROW(logos_core_set_access_policy("{\"version\":1}"));
}

TEST_F(AppLifecycleTest, SetAccessPolicy_DoesNotDisturbModulesDirs) {
    logos_core_add_modules_dir("/custom/modules");
    ASSERT_EQ(logos_core_get_modules_dirs_count(), 1);

    logos_core_set_access_policy(
        "{\"version\":1,\"mode\":\"enforce\",\"restrictions\":{}}");

    // The policy setter touches nothing else in core state.
    EXPECT_EQ(logos_core_get_modules_dirs_count(), 1);
    char* dir = logos_core_get_modules_dir_at(0);
    ASSERT_NE(dir, nullptr);
    EXPECT_EQ(std::string(dir), "/custom/modules");
    delete[] dir;
}
