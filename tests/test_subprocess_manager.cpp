// =============================================================================
// Tests for SubprocessManager lifecycle, exposed via qt_test_adapter.h.
//
// The subprocess manager maintains a registry of named child processes and
// provides token-IPC (sendToken / receive) between the core and plugin
// host processes. These tests verify:
//   - register / hasProcess / clearAll lifecycle
//   - registerProcess is idempotent
//   - get_process_id returns -1 for placeholder (not-yet-started) entries
//   - start_process actually starts a real child (uses /bin/sleep)
//   - get_process_id returns a valid PID after a real start
//   - terminate_process removes the entry
//   - terminateAll removes all entries
//   - separate names don't collide
// =============================================================================
#include <gtest/gtest.h>
#include "logos_core.h"
#include "qt_test_adapter.h"
#include <cstdint>
#include <string>
#include <thread>
#include <chrono>
#include <unistd.h>

static void clearProcessState() {
    logos_core_clear_processes();
}

class SubprocessManagerTest : public ::testing::Test {
protected:
    void SetUp() override { clearProcessState(); }
    void TearDown() override { clearProcessState(); }
};

// ---------------------------------------------------------------------------
// register / hasProcess / clear lifecycle
// ---------------------------------------------------------------------------

TEST_F(SubprocessManagerTest, RegisterProcess_HasProcessReturnsTrue) {
    logos_core_register_process("my_plugin");
    EXPECT_EQ(logos_core_has_process("my_plugin"), 1);
}

TEST_F(SubprocessManagerTest, HasProcess_ReturnsFalseForUnregistered) {
    EXPECT_EQ(logos_core_has_process("nope"), 0);
}

TEST_F(SubprocessManagerTest, ClearAll_RemovesAllEntries) {
    logos_core_register_process("p1");
    logos_core_register_process("p2");
    logos_core_register_process("p3");

    logos_core_clear_processes();

    EXPECT_EQ(logos_core_has_process("p1"), 0);
    EXPECT_EQ(logos_core_has_process("p2"), 0);
    EXPECT_EQ(logos_core_has_process("p3"), 0);
}

TEST_F(SubprocessManagerTest, RegisterProcess_IsIdempotent) {
    logos_core_register_process("dup");
    logos_core_register_process("dup");
    logos_core_register_process("dup");

    // Still registered, and clearing removes it cleanly.
    EXPECT_EQ(logos_core_has_process("dup"), 1);
    logos_core_clear_processes();
    EXPECT_EQ(logos_core_has_process("dup"), 0);
}

TEST_F(SubprocessManagerTest, NullName_DoesNotCrash) {
    logos_core_register_process(nullptr);
    EXPECT_EQ(logos_core_has_process(nullptr), 0);
}

// ---------------------------------------------------------------------------
// get_process_id: placeholder entry returns -1
// ---------------------------------------------------------------------------

TEST_F(SubprocessManagerTest, GetProcessId_ReturnsNegativeOneForPlaceholder) {
    logos_core_register_process("placeholder");
    // Placeholder has no real process — should return -1.
    int64_t pid = logos_core_get_process_id("placeholder");
    EXPECT_EQ(pid, -1);
}

TEST_F(SubprocessManagerTest, GetProcessId_ReturnsNegativeOneForUnknown) {
    int64_t pid = logos_core_get_process_id("unknown");
    EXPECT_EQ(pid, -1);
}

// ---------------------------------------------------------------------------
// start_process / get_process_id / terminate_process
// ---------------------------------------------------------------------------

// Helper: find sleep binary (macOS and Linux put it in different places)
static const char* sleepBinary() {
    if (access("/bin/sleep", X_OK) == 0) return "/bin/sleep";
    if (access("/usr/bin/sleep", X_OK) == 0) return "/usr/bin/sleep";
    return nullptr;
}

TEST_F(SubprocessManagerTest, StartProcess_ReturnsOneOnSuccess) {
    const char* sleep = sleepBinary();
    if (!sleep) GTEST_SKIP() << "sleep binary not found";

    const char* args[] = {"5", nullptr};
    int ok = logos_core_start_process("sleep_test", sleep, args);
    EXPECT_EQ(ok, 1);
}

TEST_F(SubprocessManagerTest, StartProcess_HasProcessReturnsTrueAfterStart) {
    const char* sleep = sleepBinary();
    if (!sleep) GTEST_SKIP() << "sleep binary not found";

    const char* args[] = {"5", nullptr};
    logos_core_start_process("sleep_has", sleep, args);
    EXPECT_EQ(logos_core_has_process("sleep_has"), 1);
}

TEST_F(SubprocessManagerTest, StartProcess_GetProcessIdReturnsValidPid) {
    const char* sleep = sleepBinary();
    if (!sleep) GTEST_SKIP() << "sleep binary not found";

    const char* args[] = {"5", nullptr};
    logos_core_start_process("sleep_pid", sleep, args);

    int64_t pid = logos_core_get_process_id("sleep_pid");
    EXPECT_GT(pid, 0) << "started process must have a positive PID";
}

TEST_F(SubprocessManagerTest, StartProcess_ReturnsFalseForNonexistentExecutable) {
    const char* args[] = {nullptr};
    int ok = logos_core_start_process("bad_exec",
                                       "/nonexistent/binary_that_does_not_exist",
                                       args);
    EXPECT_EQ(ok, 0);
}

TEST_F(SubprocessManagerTest, TerminateProcess_RemovesEntry) {
    const char* sleep = sleepBinary();
    if (!sleep) GTEST_SKIP() << "sleep binary not found";

    const char* args[] = {"5", nullptr};
    logos_core_start_process("sleep_term", sleep, args);
    ASSERT_EQ(logos_core_has_process("sleep_term"), 1);

    logos_core_terminate_process("sleep_term");

    EXPECT_EQ(logos_core_has_process("sleep_term"), 0);
}

TEST_F(SubprocessManagerTest, TerminateProcess_NoopForUnknownName) {
    // Should not crash when terminating a name that was never registered.
    logos_core_terminate_process("i_do_not_exist");
    SUCCEED();
}

TEST_F(SubprocessManagerTest, TerminateProcess_NoopForNullName) {
    logos_core_terminate_process(nullptr);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Multiple distinct processes coexist without colliding
// ---------------------------------------------------------------------------

TEST_F(SubprocessManagerTest, MultipleProcesses_DistinctPids) {
    const char* sleep = sleepBinary();
    if (!sleep) GTEST_SKIP() << "sleep binary not found";

    const char* args[] = {"5", nullptr};
    logos_core_start_process("proc_a", sleep, args);
    logos_core_start_process("proc_b", sleep, args);

    int64_t pidA = logos_core_get_process_id("proc_a");
    int64_t pidB = logos_core_get_process_id("proc_b");

    EXPECT_GT(pidA, 0);
    EXPECT_GT(pidB, 0);
    EXPECT_NE(pidA, pidB) << "two separate processes must have different PIDs";
}

TEST_F(SubprocessManagerTest, MultipleProcesses_TerminateOneKeepsOther) {
    const char* sleep = sleepBinary();
    if (!sleep) GTEST_SKIP() << "sleep binary not found";

    const char* args[] = {"5", nullptr};
    logos_core_start_process("keep_me", sleep, args);
    logos_core_start_process("kill_me", sleep, args);

    logos_core_terminate_process("kill_me");

    EXPECT_EQ(logos_core_has_process("kill_me"), 0);
    EXPECT_EQ(logos_core_has_process("keep_me"), 1);
}

// ---------------------------------------------------------------------------
// terminateAll removes all running processes
// ---------------------------------------------------------------------------

TEST_F(SubprocessManagerTest, TerminateAll_RemovesAllRunningProcesses) {
    const char* sleep = sleepBinary();
    if (!sleep) GTEST_SKIP() << "sleep binary not found";

    const char* args[] = {"5", nullptr};
    logos_core_start_process("ta_1", sleep, args);
    logos_core_start_process("ta_2", sleep, args);
    logos_core_register_process("ta_placeholder");

    logos_core_terminate_all();

    EXPECT_EQ(logos_core_has_process("ta_1"), 0);
    EXPECT_EQ(logos_core_has_process("ta_2"), 0);
    EXPECT_EQ(logos_core_has_process("ta_placeholder"), 0);
}
