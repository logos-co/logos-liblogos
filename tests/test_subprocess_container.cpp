// =============================================================================
// Tests for SubprocessContainer — the ModuleContainer implementation that
// manages child processes via Boost.Process v2.
//
// Verifies the container interface methods (launch, sendToken, terminate,
// hasModule, pid, getAllPids) independently from any ModuleLoader.
// =============================================================================
#include <gtest/gtest.h>
#include "containers/subprocess/subprocess_container.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class SubprocessContainerTest : public ::testing::Test {
protected:
    SubprocessContainer container;

    void SetUp() override { SubprocessContainer::clearAll(); }
    void TearDown() override { SubprocessContainer::clearAll(); }
};

static const char* sleepBinary() {
    if (access("/bin/sleep", X_OK) == 0) return "/bin/sleep";
    if (access("/usr/bin/sleep", X_OK) == 0) return "/usr/bin/sleep";
    return nullptr;
}

// ---------------------------------------------------------------------------
// canHandle: subprocess container accepts any module descriptor
// ---------------------------------------------------------------------------

TEST_F(SubprocessContainerTest, CanHandle_AcceptsAnyDescriptor) {
    LogosCore::ModuleDescriptor desc;
    desc.format = "qt-plugin";
    EXPECT_TRUE(container.canHandle(desc));

    desc.format = "wasm";
    EXPECT_TRUE(container.canHandle(desc));

    desc.format = "";
    EXPECT_TRUE(container.canHandle(desc));
}

TEST_F(SubprocessContainerTest, Id_ReturnsSubprocess) {
    EXPECT_EQ(container.id(), "subprocess");
}

// ---------------------------------------------------------------------------
// launch: spawns a real child and populates the handle
// ---------------------------------------------------------------------------

TEST_F(SubprocessContainerTest, Launch_StartsProcessAndPopulatesHandle) {
    const char* sleep = sleepBinary();
    if (!sleep) GTEST_SKIP() << "sleep binary not found";

    LogosCore::ModuleDescriptor desc;
    desc.name = "launch_test";
    LogosCore::LoadedModuleHandle handle;

    bool ok = container.launch(desc, sleep, {"5"}, nullptr, handle);
    EXPECT_TRUE(ok);
    EXPECT_EQ(handle.name, "launch_test");
    EXPECT_GT(handle.pid, 0);
}

TEST_F(SubprocessContainerTest, Launch_HasModuleReturnsTrueAfterLaunch) {
    const char* sleep = sleepBinary();
    if (!sleep) GTEST_SKIP() << "sleep binary not found";

    LogosCore::ModuleDescriptor desc;
    desc.name = "has_mod";
    LogosCore::LoadedModuleHandle handle;

    container.launch(desc, sleep, {"5"}, nullptr, handle);
    EXPECT_TRUE(container.hasModule("has_mod"));
}

TEST_F(SubprocessContainerTest, Launch_PidReturnsPidAfterLaunch) {
    const char* sleep = sleepBinary();
    if (!sleep) GTEST_SKIP() << "sleep binary not found";

    LogosCore::ModuleDescriptor desc;
    desc.name = "pid_mod";
    LogosCore::LoadedModuleHandle handle;

    container.launch(desc, sleep, {"5"}, nullptr, handle);
    auto pid = container.pid("pid_mod");
    ASSERT_TRUE(pid.has_value());
    EXPECT_GT(*pid, 0);
    EXPECT_EQ(*pid, handle.pid);
}

TEST_F(SubprocessContainerTest, Launch_FailsForNonexistentBinary) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "bad_launch";
    LogosCore::LoadedModuleHandle handle;

    bool ok = container.launch(desc, "/nonexistent/binary", {}, nullptr, handle);
    EXPECT_FALSE(ok);
}

// ---------------------------------------------------------------------------
// terminate
// ---------------------------------------------------------------------------

TEST_F(SubprocessContainerTest, Terminate_RemovesModule) {
    const char* sleep = sleepBinary();
    if (!sleep) GTEST_SKIP() << "sleep binary not found";

    LogosCore::ModuleDescriptor desc;
    desc.name = "term_mod";
    LogosCore::LoadedModuleHandle handle;

    container.launch(desc, sleep, {"5"}, nullptr, handle);
    ASSERT_TRUE(container.hasModule("term_mod"));

    container.terminate("term_mod");
    EXPECT_FALSE(container.hasModule("term_mod"));
}

TEST_F(SubprocessContainerTest, TerminateAll_RemovesAllModules) {
    const char* sleep = sleepBinary();
    if (!sleep) GTEST_SKIP() << "sleep binary not found";

    LogosCore::ModuleDescriptor desc1;
    desc1.name = "ta_1";
    LogosCore::LoadedModuleHandle h1;
    container.launch(desc1, sleep, {"5"}, nullptr, h1);

    LogosCore::ModuleDescriptor desc2;
    desc2.name = "ta_2";
    LogosCore::LoadedModuleHandle h2;
    container.launch(desc2, sleep, {"5"}, nullptr, h2);

    container.terminateAll();

    EXPECT_FALSE(container.hasModule("ta_1"));
    EXPECT_FALSE(container.hasModule("ta_2"));
}

// ---------------------------------------------------------------------------
// getAllPids
// ---------------------------------------------------------------------------

TEST_F(SubprocessContainerTest, GetAllPids_ReturnsAllRunningPids) {
    const char* sleep = sleepBinary();
    if (!sleep) GTEST_SKIP() << "sleep binary not found";

    LogosCore::ModuleDescriptor d1;
    d1.name = "gp_a";
    LogosCore::LoadedModuleHandle h1;
    container.launch(d1, sleep, {"5"}, nullptr, h1);

    LogosCore::ModuleDescriptor d2;
    d2.name = "gp_b";
    LogosCore::LoadedModuleHandle h2;
    container.launch(d2, sleep, {"5"}, nullptr, h2);

    auto pids = container.getAllPids();
    EXPECT_EQ(pids.size(), 2u);
    EXPECT_GT(pids.at("gp_a"), 0);
    EXPECT_GT(pids.at("gp_b"), 0);
    EXPECT_NE(pids.at("gp_a"), pids.at("gp_b"));
}

// ---------------------------------------------------------------------------
// onOutput callback via launch
// ---------------------------------------------------------------------------

TEST_F(SubprocessContainerTest, Launch_OutputCallbackReceivesStdoutAndStderr) {
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::pair<std::string, bool>> lines;
    std::atomic<bool> terminated{false};

    LogosCore::ModuleDescriptor desc;
    desc.name = "output_test";
    LogosCore::LoadedModuleHandle handle;

    // We can't easily capture onOutput through the container interface alone
    // since launch() sets its own callbacks. Instead, use the low-level API.
    SubprocessContainer::ProcessCallbacks cb;
    cb.onOutput = [&](const std::string&, const std::string& line, bool isStderr) {
        std::lock_guard<std::mutex> lock(mtx);
        lines.emplace_back(line, isStderr);
        cv.notify_all();
    };
    cb.onFinished = [&](const std::string&, int, bool) {
        terminated.store(true);
        cv.notify_all();
    };

    ASSERT_TRUE(SubprocessContainer::startProcess(
        "output_test", "/bin/sh", {"-c", "echo out-line; echo err-line >&2"}, cb));

    std::unique_lock<std::mutex> lock(mtx);
    cv.wait_for(lock, std::chrono::seconds(5),
                [&]() { return terminated.load() && lines.size() >= 2; });

    ASSERT_GE(lines.size(), 2u);
    bool saw_stdout = false, saw_stderr = false;
    for (auto& [line, isStderr] : lines) {
        if (!isStderr && line == "out-line") saw_stdout = true;
        if ( isStderr && line == "err-line") saw_stderr = true;
    }
    EXPECT_TRUE(saw_stdout);
    EXPECT_TRUE(saw_stderr);
}

// ---------------------------------------------------------------------------
// Crash detection — the kernel of crash isolation
// ---------------------------------------------------------------------------
//
// When a child dies on a signal, the container must surface it via
// onFinished(name, exit_code, crashed=true) and tear down its own
// bookkeeping. Every observer above (composite_runtime → module_manager's
// onTerminated → registry.markUnloaded → daemon → CLI) hangs off this
// callback — if it stops firing with crashed=true, isolation breaks
// silently. The end-to-end test in logos-logoscore-cli covers the full
// stack at ~13s; this one pins the mechanism at its source in ~50ms.

TEST_F(SubprocessContainerTest, Launch_OnFinishedReportsCrashedOnSignal) {
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> fired{false};
    std::string gotName;
    int gotExitCode = -1;
    bool gotCrashed = false;

    SubprocessContainer::ProcessCallbacks cb;
    cb.onFinished = [&](const std::string& n, int code, bool crashed) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            gotName = n;
            gotExitCode = code;
            gotCrashed = crashed;
        }
        fired.store(true);
        cv.notify_all();
    };

    // `kill -SEGV $$` makes the shell signal itself — a portable way to
    // exercise WIFSIGNALED without depending on /bin/kill -s SEGV syntax
    // or shipping a custom helper binary.
    ASSERT_TRUE(SubprocessContainer::startProcess(
        "crash_test", "/bin/sh", {"-c", "kill -SEGV $$"}, cb));

    std::unique_lock<std::mutex> lock(mtx);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                            [&]() { return fired.load(); }))
        << "onFinished never fired for a signaled child";

    EXPECT_EQ(gotName, "crash_test");
    EXPECT_TRUE(gotCrashed)
        << "container must report crashed=true on signal-exit (WIFSIGNALED). "
           "Without this, the markUnloaded propagation up the stack stalls "
           "and a crashed module looks 'loaded' forever.";
    EXPECT_EQ(gotExitCode, SIGSEGV)
        << "exit_code must carry the signal number (WTERMSIG), not the "
           "raw waitpid status. Got " << gotExitCode << ".";

    // Bookkeeping side of the contract: once onFinished has fired the
    // container must no longer claim the module exists. This is what
    // feeds upstream observers via their own ProcessEntry cleanup.
    EXPECT_FALSE(container.hasModule("crash_test"));
    EXPECT_FALSE(container.pid("crash_test").has_value());
}

// ---------------------------------------------------------------------------
// pid/hasModule for unknown names
// ---------------------------------------------------------------------------

TEST_F(SubprocessContainerTest, Pid_ReturnsNulloptForUnknown) {
    EXPECT_FALSE(container.pid("nonexistent").has_value());
}

TEST_F(SubprocessContainerTest, HasModule_ReturnsFalseForUnknown) {
    EXPECT_FALSE(container.hasModule("nonexistent"));
}

TEST_F(SubprocessContainerTest, Terminate_NoopForUnknown) {
    container.terminate("nonexistent");
    SUCCEED();
}
