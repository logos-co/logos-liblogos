// =============================================================================
// Tests for QtProcessManager — the component that spawns logos_host children,
// tracks their PIDs, routes stdout/stderr line callbacks, and handles
// termination. These tests establish the behavioural contract that any
// replacement (e.g. Boost.Process) must preserve.
//
// The tests use ordinary POSIX commands (sleep, echo, cat, sh -c) rather than
// a real logos_host binary so they can run anywhere the Nix stdenv is
// available.
// =============================================================================
#include <gtest/gtest.h>
#include "qt/qt_process_manager.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QEventLoop>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace {

// Pump Qt events until `pred` returns true or `timeoutMs` elapses. Returns
// true if the predicate became true within the timeout. Essential because
// QProcess delivers finished/errorOccurred/readyRead via the event loop.
template <typename Predicate>
bool waitUntil(Predicate pred, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!pred()) {
        if (timer.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
    // Drain any stragglers (e.g. the final readyRead triggered by process exit)
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return true;
}

// Thread-safe accumulator used by the output callback tests.
struct OutputCollector {
    std::mutex mutex;
    std::vector<std::string> lines;
    std::vector<bool> isStderrFlags;

    void append(const std::string& line, bool isStderr) {
        std::lock_guard<std::mutex> lock(mutex);
        lines.push_back(line);
        isStderrFlags.push_back(isStderr);
    }
    size_t size() {
        std::lock_guard<std::mutex> lock(mutex);
        return lines.size();
    }
    std::vector<std::string> snapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        return lines;
    }
};

// Thread-safe finish-callback record.
struct FinishRecord {
    std::mutex mutex;
    std::atomic<int> callCount{0};
    int lastExitCode = -999;
    bool lastCrashed = false;
    std::string lastName;

    void record(const std::string& name, int exitCode, bool crashed) {
        std::lock_guard<std::mutex> lock(mutex);
        lastName = name;
        lastExitCode = exitCode;
        lastCrashed = crashed;
        ++callCount;
    }
};

} // namespace

class ProcessManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // clearAll() tears down any processes leaked by a previous test
        // without firing callbacks (which could run on a dangling `this`).
        QtProcessManager::clearAll();
    }
    void TearDown() override {
        QtProcessManager::clearAll();
    }
};

// =============================================================================
// Happy-path start / terminate
// =============================================================================

TEST_F(ProcessManagerTest, StartProcess_SpawnsRunningChild) {
    QtProcessManager::ProcessCallbacks cb;
    bool started = QtProcessManager::startProcess(
        "sleeper", "sleep", {"5"}, cb);

    ASSERT_TRUE(started);
    EXPECT_TRUE(QtProcessManager::hasProcess("sleeper"));
    EXPECT_GT(QtProcessManager::getProcessId("sleeper"), 0);
}

TEST_F(ProcessManagerTest, StartProcess_ReturnsFalseForMissingExecutable) {
    QtProcessManager::ProcessCallbacks cb;
    bool started = QtProcessManager::startProcess(
        "ghost", "/definitely/does/not/exist/binary", {}, cb);

    EXPECT_FALSE(started);
    EXPECT_FALSE(QtProcessManager::hasProcess("ghost"));
    EXPECT_EQ(QtProcessManager::getProcessId("ghost"), -1);
}

TEST_F(ProcessManagerTest, StartProcess_RegistersPidInGetAllProcessIds) {
    QtProcessManager::ProcessCallbacks cb;
    ASSERT_TRUE(QtProcessManager::startProcess("p1", "sleep", {"5"}, cb));
    ASSERT_TRUE(QtProcessManager::startProcess("p2", "sleep", {"5"}, cb));

    auto pids = QtProcessManager::getAllProcessIds();
    ASSERT_EQ(pids.size(), 2u);
    EXPECT_GT(pids["p1"], 0);
    EXPECT_GT(pids["p2"], 0);
    EXPECT_NE(pids["p1"], pids["p2"]);
}

TEST_F(ProcessManagerTest, TerminateProcess_RemovesFromRegistry) {
    QtProcessManager::ProcessCallbacks cb;
    ASSERT_TRUE(QtProcessManager::startProcess("tmp", "sleep", {"5"}, cb));
    ASSERT_TRUE(QtProcessManager::hasProcess("tmp"));

    QtProcessManager::terminateProcess("tmp");

    EXPECT_FALSE(QtProcessManager::hasProcess("tmp"));
    EXPECT_EQ(QtProcessManager::getProcessId("tmp"), -1);
}

TEST_F(ProcessManagerTest, TerminateProcess_IsIdempotent) {
    QtProcessManager::ProcessCallbacks cb;
    ASSERT_TRUE(QtProcessManager::startProcess("tmp", "sleep", {"5"}, cb));

    QtProcessManager::terminateProcess("tmp");
    // Second call must be a no-op, not a crash.
    EXPECT_NO_THROW(QtProcessManager::terminateProcess("tmp"));
    EXPECT_NO_THROW(QtProcessManager::terminateProcess("never_existed"));
}

TEST_F(ProcessManagerTest, TerminateAll_ClearsEverything) {
    QtProcessManager::ProcessCallbacks cb;
    ASSERT_TRUE(QtProcessManager::startProcess("a", "sleep", {"5"}, cb));
    ASSERT_TRUE(QtProcessManager::startProcess("b", "sleep", {"5"}, cb));
    ASSERT_TRUE(QtProcessManager::startProcess("c", "sleep", {"5"}, cb));
    ASSERT_EQ(QtProcessManager::getAllProcessIds().size(), 3u);

    QtProcessManager::terminateAll();

    EXPECT_EQ(QtProcessManager::getAllProcessIds().size(), 0u);
    EXPECT_FALSE(QtProcessManager::hasProcess("a"));
    EXPECT_FALSE(QtProcessManager::hasProcess("b"));
    EXPECT_FALSE(QtProcessManager::hasProcess("c"));
}

// =============================================================================
// Natural exit — onFinished callback must fire exactly once, with crashed=false
// =============================================================================

TEST_F(ProcessManagerTest, OnFinished_FiresOnceOnNaturalExit) {
    auto record = std::make_shared<FinishRecord>();

    QtProcessManager::ProcessCallbacks cb;
    cb.onFinished = [record](const std::string& name, int exitCode, bool crashed) {
        record->record(name, exitCode, crashed);
    };

    // Sleep briefly so the `finished` signal connection runs before the
    // child actually exits — qt_process_manager connects the handler AFTER
    // waitForStarted returns, so zero-duration children race the connect.
    ASSERT_TRUE(QtProcessManager::startProcess(
        "quick", "sh", {"-c", "sleep 0.3; exit 0"}, cb));

    ASSERT_TRUE(waitUntil([&]() { return record->callCount.load() >= 1; }, 5000))
        << "onFinished was not invoked within 5s";

    EXPECT_EQ(record->callCount.load(), 1);
    EXPECT_EQ(record->lastName, "quick");
    EXPECT_EQ(record->lastExitCode, 0);
    EXPECT_FALSE(record->lastCrashed);
    EXPECT_FALSE(QtProcessManager::hasProcess("quick"));
}

TEST_F(ProcessManagerTest, OnFinished_ReflectsNonZeroExitCode) {
    auto record = std::make_shared<FinishRecord>();

    QtProcessManager::ProcessCallbacks cb;
    cb.onFinished = [record](const std::string& name, int exitCode, bool crashed) {
        record->record(name, exitCode, crashed);
    };

    ASSERT_TRUE(QtProcessManager::startProcess(
        "exit7", "sh", {"-c", "sleep 0.3; exit 7"}, cb));

    ASSERT_TRUE(waitUntil([&]() { return record->callCount.load() >= 1; }));
    EXPECT_EQ(record->lastExitCode, 7);
    EXPECT_FALSE(record->lastCrashed) << "non-zero exit is not a crash";
}

// =============================================================================
// Crash detection — a child killed by a signal must report crashed=true
// =============================================================================

TEST_F(ProcessManagerTest, OnFinished_ReportsCrashWhenChildKilledBySignal) {
    auto record = std::make_shared<FinishRecord>();

    QtProcessManager::ProcessCallbacks cb;
    cb.onFinished = [record](const std::string& name, int exitCode, bool crashed) {
        record->record(name, exitCode, crashed);
    };

    // Short sleep first so the finished-signal connection runs before the
    // child is killed. Then the shell kills itself with SIGKILL, which Qt
    // reports as CrashExit -> crashed=true.
    ASSERT_TRUE(QtProcessManager::startProcess(
        "crasher", "sh", {"-c", "sleep 0.3; kill -9 $$"}, cb));

    ASSERT_TRUE(waitUntil([&]() { return record->callCount.load() >= 1; }));
    EXPECT_TRUE(record->lastCrashed)
        << "sh killed by SIGKILL should report crashed=true";
}

// =============================================================================
// stdout line-by-line parsing
//
// The production callback in plugin_launcher.cpp scans child output for
// "Warning:" / "Critical:" / "ERROR:" substrings; a replacement implementation
// that delivers partial lines or concatenated lines would break that scanning
// silently. These tests lock the contract: one callback per newline-delimited
// line, line content does not include the trailing '\n', empty trailing lines
// are suppressed.
// =============================================================================

TEST_F(ProcessManagerTest, OnOutput_DeliversOneCallbackPerLine) {
    auto collector = std::make_shared<OutputCollector>();
    auto record = std::make_shared<FinishRecord>();

    QtProcessManager::ProcessCallbacks cb;
    cb.onOutput = [collector](const std::string& /*name*/,
                              const std::string& line, bool isStderr) {
        collector->append(line, isStderr);
    };
    cb.onFinished = [record](const std::string& name, int code, bool crashed) {
        record->record(name, code, crashed);
    };

    // Emit three distinct lines, then sleep briefly before exiting so the
    // finished-signal connection is in place before the child exits.
    ASSERT_TRUE(QtProcessManager::startProcess(
        "three_lines", "sh",
        {"-c", "printf 'first\\nsecond\\nthird\\n'; sleep 0.3"}, cb));

    ASSERT_TRUE(waitUntil([&]() { return record->callCount.load() >= 1; }));
    auto lines = collector->snapshot();
    ASSERT_EQ(lines.size(), 3u) << "expected 3 lines, got " << lines.size();
    EXPECT_EQ(lines[0], "first");
    EXPECT_EQ(lines[1], "second");
    EXPECT_EQ(lines[2], "third");
}

TEST_F(ProcessManagerTest, OnOutput_DoesNotIncludeTrailingNewline) {
    auto collector = std::make_shared<OutputCollector>();
    auto record = std::make_shared<FinishRecord>();

    QtProcessManager::ProcessCallbacks cb;
    cb.onOutput = [collector](const std::string&, const std::string& line, bool) {
        collector->append(line, false);
    };
    cb.onFinished = [record](const std::string& n, int c, bool cr) {
        record->record(n, c, cr);
    };

    ASSERT_TRUE(QtProcessManager::startProcess(
        "with_newline", "sh", {"-c", "printf 'hello world\\n'; sleep 0.3"}, cb));

    ASSERT_TRUE(waitUntil([&]() { return record->callCount.load() >= 1; }));
    auto lines = collector->snapshot();
    ASSERT_GE(lines.size(), 1u);
    EXPECT_EQ(lines[0], "hello world");
    for (const auto& l : lines) {
        EXPECT_EQ(l.find('\n'), std::string::npos)
            << "callback line must not contain '\\n'";
    }
}

TEST_F(ProcessManagerTest, OnOutput_PreservesWarningErrorKeywords) {
    // Regression: plugin_launcher.cpp does substring matching on
    // "Warning:" / "Critical:" / "ERROR:" in forwarded child output to
    // classify the log level. If a replacement implementation reorders,
    // concatenates, or splits these keywords mid-line, that classification
    // silently breaks.
    auto collector = std::make_shared<OutputCollector>();
    auto record = std::make_shared<FinishRecord>();

    QtProcessManager::ProcessCallbacks cb;
    cb.onOutput = [collector](const std::string&, const std::string& line, bool) {
        collector->append(line, false);
    };
    cb.onFinished = [record](const std::string& n, int c, bool cr) {
        record->record(n, c, cr);
    };

    ASSERT_TRUE(QtProcessManager::startProcess(
        "keywords", "sh",
        {"-c",
         "printf 'Warning: this is a warning\\nCritical: this is critical\\nERROR: this is an error\\n'; sleep 0.3"},
        cb));

    ASSERT_TRUE(waitUntil([&]() { return record->callCount.load() >= 1; }));
    auto lines = collector->snapshot();
    ASSERT_EQ(lines.size(), 3u);

    // Each keyword must arrive contiguously in a single callback invocation.
    bool sawWarning = false, sawCritical = false, sawError = false;
    for (const auto& l : lines) {
        if (l.find("Warning:") != std::string::npos) sawWarning = true;
        if (l.find("Critical:") != std::string::npos) sawCritical = true;
        if (l.find("ERROR:") != std::string::npos) sawError = true;
    }
    EXPECT_TRUE(sawWarning);
    EXPECT_TRUE(sawCritical);
    EXPECT_TRUE(sawError);
}

// =============================================================================
// Concurrent start — all processes must be tracked, no PID collisions.
// =============================================================================

TEST_F(ProcessManagerTest, StartMany_EachTrackedWithUniquePid) {
    QtProcessManager::ProcessCallbacks cb;

    const int N = 5;
    for (int i = 0; i < N; ++i) {
        std::string name = "worker_" + std::to_string(i);
        ASSERT_TRUE(QtProcessManager::startProcess(name, "sleep", {"10"}, cb))
            << "failed to start " << name;
    }

    auto pids = QtProcessManager::getAllProcessIds();
    ASSERT_EQ(pids.size(), static_cast<size_t>(N));

    // All PIDs must be unique.
    std::vector<int64_t> seen;
    for (auto& kv : pids) {
        EXPECT_GT(kv.second, 0);
        EXPECT_TRUE(std::find(seen.begin(), seen.end(), kv.second) == seen.end())
            << "duplicate PID " << kv.second << " for " << kv.first;
        seen.push_back(kv.second);
    }
}

// =============================================================================
// Natural-exit cleanup — after a short-lived process finishes, the registry
// entry must be removed so subsequent hasProcess returns false.
// =============================================================================

TEST_F(ProcessManagerTest, NaturalExit_RemovesEntryFromRegistry) {
    auto record = std::make_shared<FinishRecord>();
    QtProcessManager::ProcessCallbacks cb;
    cb.onFinished = [record](const std::string& n, int c, bool cr) {
        record->record(n, c, cr);
    };

    ASSERT_TRUE(QtProcessManager::startProcess(
        "auto_exit", "sh", {"-c", "sleep 0.3; exit 0"}, cb));
    ASSERT_TRUE(waitUntil([&]() { return record->callCount.load() >= 1; }));

    EXPECT_FALSE(QtProcessManager::hasProcess("auto_exit"))
        << "after natural exit, process must be removed from registry";
    EXPECT_EQ(QtProcessManager::getProcessId("auto_exit"), -1);
}

// =============================================================================
// Stress: many load/terminate cycles must not leak PIDs or grow state.
// =============================================================================

TEST_F(ProcessManagerTest, StressLoop_StartTerminateCycles) {
    QtProcessManager::ProcessCallbacks cb;

    const int cycles = 20;
    for (int i = 0; i < cycles; ++i) {
        std::string name = "cycle_" + std::to_string(i);
        ASSERT_TRUE(QtProcessManager::startProcess(name, "sleep", {"30"}, cb))
            << "cycle " << i << " failed to start";
        EXPECT_TRUE(QtProcessManager::hasProcess(name));
        QtProcessManager::terminateProcess(name);
        EXPECT_FALSE(QtProcessManager::hasProcess(name));
    }

    // After the whole loop, the internal map must be empty.
    EXPECT_EQ(QtProcessManager::getAllProcessIds().size(), 0u);
}

// =============================================================================
// registerProcess — placeholder entry used by sendToken's failure path. Must
// not crash and must show up via hasProcess.
// =============================================================================

TEST_F(ProcessManagerTest, RegisterProcess_CreatesNullPlaceholder) {
    QtProcessManager::registerProcess("placeholder");
    EXPECT_TRUE(QtProcessManager::hasProcess("placeholder"));
    EXPECT_EQ(QtProcessManager::getProcessId("placeholder"), -1)
        << "placeholder has no child yet, PID must be -1";

    // terminateProcess on a placeholder (null QProcess*) is a no-op in the
    // current code because the removed value is null; teardown must not crash.
    EXPECT_NO_THROW(QtProcessManager::terminateProcess("placeholder"));
}
