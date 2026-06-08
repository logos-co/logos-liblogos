// =============================================================================
// Tests for the process manager lifecycle, exposed via logos_core.h.
//
// The process manager maintains a registry of named child processes and
// provides token-IPC (sendToken / receive) between the core and module
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
#include "containers/subprocess/subprocess_manager.h"
#include "qt_test_adapter.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>       // for EINTR in the read-until-EOF loop
#include <cstdint>
#include <cstdlib>
#include <cstring>      // for strncpy
#include <mutex>
#include <poll.h>        // for poll() in the impostor accept loop
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

static void clearProcessState() {
    logos_core_clear_processes();
}

class ProcessManagerTest : public ::testing::Test {
protected:
    void SetUp() override { clearProcessState(); }
    void TearDown() override { clearProcessState(); }
};

// ---------------------------------------------------------------------------
// register / hasProcess / clear lifecycle
// ---------------------------------------------------------------------------

TEST_F(ProcessManagerTest, RegisterProcess_HasProcessReturnsTrue) {
    logos_core_register_process("my_module");
    EXPECT_EQ(logos_core_has_process("my_module"), 1);
}

TEST_F(ProcessManagerTest, HasProcess_ReturnsFalseForUnregistered) {
    EXPECT_EQ(logos_core_has_process("nope"), 0);
}

TEST_F(ProcessManagerTest, ClearAll_RemovesAllEntries) {
    logos_core_register_process("p1");
    logos_core_register_process("p2");
    logos_core_register_process("p3");

    logos_core_clear_processes();

    EXPECT_EQ(logos_core_has_process("p1"), 0);
    EXPECT_EQ(logos_core_has_process("p2"), 0);
    EXPECT_EQ(logos_core_has_process("p3"), 0);
}

TEST_F(ProcessManagerTest, RegisterProcess_IsIdempotent) {
    logos_core_register_process("dup");
    logos_core_register_process("dup");
    logos_core_register_process("dup");

    // Still registered, and clearing removes it cleanly.
    EXPECT_EQ(logos_core_has_process("dup"), 1);
    logos_core_clear_processes();
    EXPECT_EQ(logos_core_has_process("dup"), 0);
}

TEST_F(ProcessManagerTest, NullName_DoesNotCrash) {
    logos_core_register_process(nullptr);
    EXPECT_EQ(logos_core_has_process(nullptr), 0);
}

// ---------------------------------------------------------------------------
// get_process_id: placeholder entry returns -1
// ---------------------------------------------------------------------------

TEST_F(ProcessManagerTest, GetProcessId_ReturnsNegativeOneForPlaceholder) {
    logos_core_register_process("placeholder");
    // Placeholder has no real process — should return -1.
    int64_t pid = logos_core_get_process_id("placeholder");
    EXPECT_EQ(pid, -1);
}

TEST_F(ProcessManagerTest, GetProcessId_ReturnsNegativeOneForUnknown) {
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

TEST_F(ProcessManagerTest, StartProcess_ReturnsOneOnSuccess) {
    const char* sleep = sleepBinary();
    if (!sleep) GTEST_SKIP() << "sleep binary not found";

    const char* args[] = {"5", nullptr};
    int ok = logos_core_start_process("sleep_test", sleep, args);
    EXPECT_EQ(ok, 1);
}

TEST_F(ProcessManagerTest, StartProcess_HasProcessReturnsTrueAfterStart) {
    const char* sleep = sleepBinary();
    if (!sleep) GTEST_SKIP() << "sleep binary not found";

    const char* args[] = {"5", nullptr};
    logos_core_start_process("sleep_has", sleep, args);
    EXPECT_EQ(logos_core_has_process("sleep_has"), 1);
}

TEST_F(ProcessManagerTest, StartProcess_GetProcessIdReturnsValidPid) {
    const char* sleep = sleepBinary();
    if (!sleep) GTEST_SKIP() << "sleep binary not found";

    const char* args[] = {"5", nullptr};
    logos_core_start_process("sleep_pid", sleep, args);

    int64_t pid = logos_core_get_process_id("sleep_pid");
    EXPECT_GT(pid, 0) << "started process must have a positive PID";
}

TEST_F(ProcessManagerTest, StartProcess_ReturnsFalseForNonexistentExecutable) {
    const char* args[] = {nullptr};
    int ok = logos_core_start_process("bad_exec",
                                       "/nonexistent/binary_that_does_not_exist",
                                       args);
    EXPECT_EQ(ok, 0);
}

TEST_F(ProcessManagerTest, TerminateProcess_RemovesEntry) {
    const char* sleep = sleepBinary();
    if (!sleep) GTEST_SKIP() << "sleep binary not found";

    const char* args[] = {"5", nullptr};
    logos_core_start_process("sleep_term", sleep, args);
    ASSERT_EQ(logos_core_has_process("sleep_term"), 1);

    logos_core_terminate_process("sleep_term");

    EXPECT_EQ(logos_core_has_process("sleep_term"), 0);
}

TEST_F(ProcessManagerTest, TerminateProcess_NoopForUnknownName) {
    // Should not crash when terminating a name that was never registered.
    logos_core_terminate_process("i_do_not_exist");
    SUCCEED();
}

TEST_F(ProcessManagerTest, TerminateProcess_NoopForNullName) {
    logos_core_terminate_process(nullptr);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Multiple distinct processes coexist without colliding
// ---------------------------------------------------------------------------

TEST_F(ProcessManagerTest, MultipleProcesses_DistinctPids) {
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

TEST_F(ProcessManagerTest, MultipleProcesses_TerminateOneKeepsOther) {
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
// onOutput: stdout and stderr are delivered with the correct isStderr flag
// ---------------------------------------------------------------------------

TEST_F(ProcessManagerTest, StartProcess_OnOutput_SplitsStdoutAndStderr) {
    // Child writes one line to stdout and one to stderr, then exits.
    // Verify both reach onOutput with the right isStderr flag.
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::pair<std::string, bool>> lines; // (line, isStderr)
    std::atomic<bool> finished{false};

    SubprocessManager::ProcessCallbacks cb;
    cb.onOutput = [&](const std::string& /*name*/, const std::string& line, bool isStderr) {
        std::lock_guard<std::mutex> lock(mtx);
        lines.emplace_back(line, isStderr);
        cv.notify_all();
    };
    cb.onFinished = [&](const std::string& /*name*/, int /*code*/, bool /*crashed*/) {
        finished.store(true);
        cv.notify_all();
    };

    std::vector<std::string> args = {"-c", "echo out-line; echo err-line >&2"};
    ASSERT_TRUE(SubprocessManager::startProcess("dualstream", "/bin/sh", args, cb));

    std::unique_lock<std::mutex> lock(mtx);
    cv.wait_for(lock, std::chrono::seconds(5),
                [&]() { return finished.load() && lines.size() >= 2; });

    ASSERT_GE(lines.size(), 2u);
    bool saw_stdout = false, saw_stderr = false;
    for (auto& [line, isStderr] : lines) {
        if (!isStderr && line == "out-line") saw_stdout = true;
        if ( isStderr && line == "err-line") saw_stderr = true;
    }
    EXPECT_TRUE(saw_stdout) << "stdout line missing or wrongly tagged";
    EXPECT_TRUE(saw_stderr) << "stderr line missing or wrongly tagged";
}

// ---------------------------------------------------------------------------
// terminateAll removes all running processes
// ---------------------------------------------------------------------------

TEST_F(ProcessManagerTest, TerminateAll_RemovesAllRunningProcesses) {
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

// ---------------------------------------------------------------------------
// sendTokenToProcess: race against the child binding its Unix socket
//
// Real failure mode in production: the parent calls sendTokenToProcess
// immediately after fork+exec, but the child has to do dynamic loader work,
// Qt platform bring-up, CLI11 parse, and plugin loadFromPath before its
// QtTokenReceiver binds the socket inside setupModule → receiveAuthToken.
// Under load, that prelude exceeded the old 900ms budget (10 × 100ms) and
// produced "Failed to connect to token socket" + a half-loaded module.
// These tests pin down the new contract: a configurable max_wait_ms budget
// that succeeds when the socket appears within it, and fails (without
// busy-looping past the deadline) when it does not.
// ---------------------------------------------------------------------------

namespace {

std::string tokenSocketPath(const std::string& name) {
    const char* tmp = std::getenv("TMPDIR");
    std::string dir;
    if (tmp && tmp[0]) {
        dir = tmp;
        while (!dir.empty() && dir.back() == '/') dir.pop_back();
    } else {
        dir = "/tmp";
    }
    std::string socketName = "logos_token_" + name;
    const char* instanceId = std::getenv("LOGOS_INSTANCE_ID");
    if (instanceId && *instanceId) {
        socketName += "_";
        socketName += instanceId;
    }
    return dir + "/" + socketName;
}

int bindUnixListener(const std::string& path) {
    ::unlink(path.c_str());
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 1) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

TEST_F(ProcessManagerTest, SendToken_FailsFast_WhenSocketNeverAppears) {
    const std::string name = "race_no_socket";
    const std::string path = tokenSocketPath(name);
    ::unlink(path.c_str());  // ensure no stale socket

    const int budget_ms = 200;
    const auto t0 = std::chrono::steady_clock::now();
    bool ok = SubprocessManager::sendTokenToProcess(name, "tok", budget_ms);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT_FALSE(ok) << "sendTokenToProcess must fail when no socket appears";
    EXPECT_GE(elapsed_ms, budget_ms - 50) << "should respect the budget, not bail early";
    EXPECT_LE(elapsed_ms, budget_ms + 500) << "should not loop past the deadline";
}

TEST_F(ProcessManagerTest, SendToken_SucceedsAfterDelay) {
    const std::string name = "race_late_bind";
    const std::string path = tokenSocketPath(name);
    ::unlink(path.c_str());

    const int delay_ms = 300;
    const int budget_ms = 1500;

    std::atomic<bool> received{false};
    std::string receivedToken;
    std::thread binder([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        int listener = bindUnixListener(path);
        if (listener < 0) return;
        int client = ::accept(listener, nullptr, nullptr);
        if (client >= 0) {
            // Drain until EOF (sender's close()). A single ::read()
            // can return short on stream sockets even when the sender
            // wrote everything in one ::write() — kernel buffer
            // fragmentation. Loop reads accumulate the full payload.
            char buf[256];
            for (;;) {
                ssize_t n = ::read(client, buf, sizeof(buf));
                if (n > 0) {
                    receivedToken.append(buf, static_cast<std::size_t>(n));
                    continue;
                }
                if (n == 0) break;          // peer closed
                if (errno == EINTR) continue;
                break;                       // unrecoverable read error
            }
            if (!receivedToken.empty()) received.store(true);
            ::close(client);
        }
        ::close(listener);
    });

    const auto t0 = std::chrono::steady_clock::now();
    bool ok = SubprocessManager::sendTokenToProcess(name, "hello-token", budget_ms);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    binder.join();
    ::unlink(path.c_str());

    EXPECT_TRUE(ok) << "should succeed once the socket appears within the budget";
    EXPECT_GE(elapsed_ms, delay_ms - 50)
        << "must wait until the listener is bound";
    EXPECT_LT(elapsed_ms, budget_ms)
        << "must complete before exhausting the budget";
    EXPECT_TRUE(received.load());
    EXPECT_EQ(receivedToken, "hello-token");
}

// ---------------------------------------------------------------------------
// F-010 (CWE-940): the parent must NOT hand the auth token to an impostor
// that pre-binds the predictable token socket before the real child listens.
//
// Threat model: the token socket path ($TMPDIR/logos_token_<name>) is
// predictable and lives in a world-writable temp dir. A local attacker can
// bind+listen there before the legitimate child module calls listen(). If the
// parent connects and writes the token without checking who is listening, the
// secret leaks and the attacker can replay it for authorized cross-module
// calls.
//
// We replicate the squat with an in-process impostor whose listener pid is the
// test process itself, while a *real* child (a sleep) is registered for the
// same name so the container knows the genuine child's pid. The peer-pid gate
// must reject the impostor: sendTokenToProcess must NOT write the token to it
// and must fail rather than leak the secret. The gate reads the peer pid via
// SO_PEERCRED on Linux and getsockopt(SOL_LOCAL, LOCAL_PEERPID) on macOS, so
// the same in-process impostor is rejected on both platforms — its pid is the
// test process, not the registered child.
//
// Pre-fix (no listener authentication) this test FAILS: the impostor receives
// the token and `ok` is true. Post-fix it passes: the impostor receives
// nothing and `ok` is false.
//
// Limited to Linux and macOS — the two platforms with a peer-credential API
// and a pid gate. On any other platform peerIsTrusted() fails closed without
// inspecting a pid, so this in-process impostor (same uid, different pid)
// cannot exercise the pid gate the test is asserting.
// ---------------------------------------------------------------------------

#if defined(__linux__) || defined(__APPLE__)
TEST_F(ProcessManagerTest, SendToken_RejectsImpostorListenerSquattingSocket) {
    const char* sleep = sleepBinary();
    if (!sleep) GTEST_SKIP() << "sleep binary not found";

    const std::string name = "f010_impostor";
    const std::string path = tokenSocketPath(name);
    ::unlink(path.c_str());  // clear any stale socket

    // The attacker pre-binds the predictable socket path and listens. Use a
    // generous backlog and (below) a thread that accepts continuously: the
    // parent's connect() is blocking, so an un-drained backlog could wedge it
    // and mask the behaviour we're testing. We want connect() to keep
    // succeeding so the parent's *authentication* decision is what's exercised.
    int impostor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(impostor, 0);
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    ASSERT_EQ(::bind(impostor, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0)
        << "failed to bind impostor listener at " << path;
    ASSERT_EQ(::listen(impostor, 64), 0);
    struct SockGuard {
        int fd; std::string p;
        ~SockGuard() { if (fd >= 0) ::close(fd); ::unlink(p.c_str()); }
    } guard{impostor, path};

    // Spawn a real child for `name` so the container records its (genuine)
    // pid. This is what makes the impostor distinguishable: its listener pid
    // is this test process, not the registered child. (The impostor shares our
    // uid, so the uid gate alone wouldn't reject it; the pid gate is what
    // catches the squat — via SO_PEERCRED on Linux, LOCAL_PEERPID on macOS.)
    const char* args[] = {"5", nullptr};
    ASSERT_EQ(logos_core_start_process(name.c_str(), sleep, args), 1);
    const int64_t childPid = logos_core_get_process_id(name.c_str());
    ASSERT_GT(childPid, 0);
    ASSERT_NE(childPid, static_cast<int64_t>(::getpid()))
        << "impostor pid must differ from the real child pid for this test to "
           "exercise the pid gate";

    // Impostor accept loop: keep accepting (draining the backlog so the
    // parent's blocking connect() never wedges) and read whatever arrives,
    // until the main thread signals it is done sending. If the fix works the
    // parent rejects us and closes without writing, so every accepted client
    // yields zero bytes; pre-fix it writes the token and `stolen` captures it.
    std::atomic<bool> stop{false};
    std::atomic<bool> impostorGotToken{false};
    std::mutex stolenMtx;
    std::string stolen;
    std::thread thief([&]() {
        while (!stop.load()) {
            struct pollfd pfd{impostor, POLLIN, 0};
            int pr = ::poll(&pfd, 1, 50);
            if (pr <= 0 || !(pfd.revents & POLLIN)) continue;
            int client = ::accept(impostor, nullptr, nullptr);
            if (client < 0) continue;
            std::string chunk;
            char buf[256];
            for (;;) {
                ssize_t n = ::read(client, buf, sizeof(buf));
                if (n > 0) { chunk.append(buf, static_cast<std::size_t>(n)); continue; }
                if (n < 0 && errno == EINTR) continue;
                break;  // EOF or error
            }
            ::close(client);
            if (!chunk.empty()) {
                std::lock_guard<std::mutex> lk(stolenMtx);
                stolen += chunk;
                impostorGotToken.store(true);
            }
        }
    });

    const std::string secret = "f47ac10b-58cc-4372-a567-0e02b2c3d479";
    // Short budget: the legitimate child never binds (we squat the path), so
    // after the impostor is rejected sendTokenToProcess runs out its retry
    // budget and fails. Keep it small so the test is quick.
    const int budget_ms = 400;
    bool ok = SubprocessManager::sendTokenToProcess(name, secret, budget_ms);

    // Give the impostor a moment to surface any bytes the parent may have
    // written on its final connection before we tear down.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true);
    thief.join();

    std::string stolenCopy;
    { std::lock_guard<std::mutex> lk(stolenMtx); stolenCopy = stolen; }

    EXPECT_FALSE(impostorGotToken.load())
        << "SECURITY: auth token leaked to an impostor squatting the token "
           "socket — the parent wrote the secret to a listener it never "
           "authenticated (CWE-940). Stolen bytes: '" << stolenCopy << "'";
    EXPECT_NE(stolenCopy, secret)
        << "SECURITY: impostor received the exact auth token";
    EXPECT_FALSE(ok)
        << "sendTokenToProcess must fail rather than hand the token to an "
           "unauthenticated peer";
}
#endif  // __linux__ || __APPLE__
