// =============================================================================
// Tests for the token-exchange IPC between logos_core (send) and
// receive (logos_core_receive_auth_token). The two sides normally run in
// different processes; these tests exercise them in the same process using a
// worker thread for the receiver.
//
// The point of these tests is to pin the behavioural contract so any
// replacement implementation must preserve it:
//   - socket name mapping is "logos_token_<module_name>"
//   - client can retry if server is not listening yet
//   - token round-trip is byte-exact (no truncation, no trailing null)
//   - stale socket files do not prevent a new receiver from binding
//   - wrong socket name times out cleanly without hanging forever
//   - send to a never-listening server fails cleanly (no hang)
// =============================================================================
#include <gtest/gtest.h>
#include "logos_core.h"
#include "qt_test_adapter.h"
#include "peer_credentials.h"
#include "unix_socket_path.h"
#include "path_safety.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
using Ms    = std::chrono::milliseconds;

template <typename Predicate>
bool waitUntil(Predicate pred, int timeoutMs = 5000) {
    auto deadline = Clock::now() + Ms(timeoutMs);
    while (!pred()) {
        if (Clock::now() > deadline) return false;
        std::this_thread::sleep_for(Ms(5));
    }
    return true;
}

// Run the receiver in a std::thread so the sender can run on the main thread.
// logos_core_receive_auth_token() owns its own server and does not rely on the
// event loop, so running it off-thread is safe.
struct ReceiverHandle {
    std::thread thread;
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    std::string token;

    void start(const std::string& moduleName) {
        thread = std::thread([this, moduleName]() {
            started = true;
            char* raw = logos_core_receive_auth_token(moduleName.c_str());
            if (raw) {
                token = raw;
                delete[] raw;
            }
            finished = true;
        });
    }

    void join() {
        if (thread.joinable()) thread.join();
    }

    ~ReceiverHandle() { join(); }
};

} // namespace

class TokenExchangeTest : public ::testing::Test {
protected:
    void SetUp() override {
        logos_core_clear_processes();
    }
    void TearDown() override {
        logos_core_clear_processes();
    }
};

// =============================================================================
// Happy path — server listening, client sends, full token arrives.
// =============================================================================

TEST_F(TokenExchangeTest, RoundTrip_SucceedsWithServerRunningFirst) {
    const std::string moduleName = "rt_happy";
    const std::string token = "8400e3a5-1b3a-4de5-9f1f-abcdef012345";

    ReceiverHandle receiver;
    receiver.start(moduleName);

    logos_core_register_process(moduleName.c_str());

    // Give the server's listen() a moment so the first connect attempt succeeds.
    std::this_thread::sleep_for(Ms(100));

    int sent = logos_core_send_token(moduleName.c_str(), token.c_str());
    EXPECT_EQ(sent, 1);

    ASSERT_TRUE(waitUntil([&]() { return receiver.finished.load(); }, 5000))
        << "receiver did not finish within 5s";
    receiver.join();

    EXPECT_EQ(receiver.token, token)
        << "round-tripped token must be byte-exact";
}

// =============================================================================
// Retry path — client starts before the server has listen()'d.
// =============================================================================

TEST_F(TokenExchangeTest, RoundTrip_SucceedsWhenClientStartsBeforeServer) {
    const std::string moduleName = "rt_retry";
    const std::string token = "deadbeef-1234-5678-9abc-def012345678";
    logos_core_register_process(moduleName.c_str());

    std::atomic<int> sendResult{0};
    std::atomic<bool> senderDone{false};
    std::thread senderThread([&]() {
        sendResult = logos_core_send_token(moduleName.c_str(), token.c_str());
        senderDone = true;
    });

    // Delay > one retry interval so the sender definitely retries at least once.
    std::this_thread::sleep_for(Ms(250));

    ReceiverHandle receiver;
    receiver.start(moduleName);

    ASSERT_TRUE(waitUntil([&]() {
        return senderDone.load() && receiver.finished.load();
    }, 8000)) << "retry round-trip did not complete in 8s";

    senderThread.join();
    receiver.join();

    EXPECT_EQ(sendResult.load(), 1)
        << "sendToken should have succeeded via its retry loop";
    EXPECT_EQ(receiver.token, token);
}

// =============================================================================
// Byte integrity — a 36-char UUID string must round-trip with no truncation.
// =============================================================================

TEST_F(TokenExchangeTest, TokenBytes_RoundTripIsByteExact) {
    const std::string moduleName = "rt_bytes";
    const std::string token = "f47ac10b-58cc-4372-a567-0e02b2c3d479";
    logos_core_register_process(moduleName.c_str());

    ReceiverHandle receiver;
    receiver.start(moduleName);
    std::this_thread::sleep_for(Ms(100));

    ASSERT_EQ(logos_core_send_token(moduleName.c_str(), token.c_str()), 1);
    ASSERT_TRUE(waitUntil([&]() { return receiver.finished.load(); }));
    receiver.join();

    const std::string& got = receiver.token;
    EXPECT_EQ(got.size(), token.size())
        << "received token length must match sent length";
    EXPECT_EQ(got, token);
    EXPECT_EQ(got.find('\0'), std::string::npos);
    EXPECT_EQ(got.find(' '),  std::string::npos);
    EXPECT_EQ(got.find('\n'), std::string::npos);
    int dashCount = 0;
    for (char c : got) if (c == '-') ++dashCount;
    EXPECT_EQ(dashCount, 4);
}

// =============================================================================
// Stale socket file — a crashed previous run can leave a socket path on disk.
// =============================================================================

TEST_F(TokenExchangeTest, StaleSocketFile_DoesNotBlockNewReceiver) {
    const std::string moduleName = "rt_stale";
    const std::string token = "cafef00d-0000-0000-0000-000000000001";

    // Create and immediately close a socket file to simulate stale state.
    logos_core_create_stale_token_socket(moduleName.c_str());

    ReceiverHandle receiver;
    receiver.start(moduleName);

    logos_core_register_process(moduleName.c_str());
    std::this_thread::sleep_for(Ms(100));

    EXPECT_EQ(logos_core_send_token(moduleName.c_str(), token.c_str()), 1);
    ASSERT_TRUE(waitUntil([&]() { return receiver.finished.load(); }, 5000))
        << "receiver must be able to bind over a stale socket path";
    receiver.join();

    EXPECT_EQ(receiver.token, token);
}

// =============================================================================
// Wrong socket name — client connects to a name nobody listens on.
// =============================================================================

TEST_F(TokenExchangeTest, WrongName_FailsCleanlyWithinTimeout) {
    const std::string moduleName = "rt_nobody_listening";
    const std::string token = "deadbeef-dead-beef-dead-beefdeadbeef";

    logos_core_register_process(moduleName.c_str());

    auto start = Clock::now();
    int sent = logos_core_send_token(moduleName.c_str(), token.c_str());
    auto elapsed = std::chrono::duration_cast<Ms>(Clock::now() - start).count();

    EXPECT_EQ(sent, 0) << "sendToken to a nonexistent server must fail";
    // The default budget is 5000 ms; with 50 ms polls the deadline check
    // can overshoot by ~one poll interval before the loop exits, plus
    // syscall and scheduler slack. 5500 ms is the budget + a single
    // generous poll's worth of margin — far below the next-larger
    // budget anyone reasonably picks (10 s+).
    EXPECT_LT(elapsed, 5500)
        << "sendToken must give up within its retry budget + slack, got "
        << elapsed << "ms";

    // The failure path removes the placeholder entry.
    EXPECT_EQ(logos_core_has_process(moduleName.c_str()), 0);
}

// =============================================================================
// Receiver timeout — start a receiver for a module name, never send a token.
// =============================================================================

TEST_F(TokenExchangeTest, Receiver_TimesOutWhenNoClientConnects) {
    const std::string moduleName = "rt_no_client";

    ReceiverHandle receiver;
    auto start = Clock::now();
    receiver.start(moduleName);

    ASSERT_TRUE(waitUntil([&]() { return receiver.finished.load(); }, 15000))
        << "receiver did not return within its own timeout";
    receiver.join();
    auto elapsed = std::chrono::duration_cast<Ms>(Clock::now() - start).count();

    EXPECT_TRUE(receiver.token.empty())
        << "no client connected; received token must be empty";
    EXPECT_LT(elapsed, 15000)
        << "receiver hung past its documented 10s timeout";
}

// =============================================================================
// Concurrent round-trips on distinct module names must not interfere.
// =============================================================================

TEST_F(TokenExchangeTest, ConcurrentDistinctModules_EachRoundTripsCorrectly) {
    const std::string nameA = "rt_a";
    const std::string nameB = "rt_b";
    const std::string tokenA = "11111111-1111-1111-1111-111111111111";
    const std::string tokenB = "22222222-2222-2222-2222-222222222222";

    logos_core_register_process(nameA.c_str());
    logos_core_register_process(nameB.c_str());

    ReceiverHandle recvA;
    ReceiverHandle recvB;
    recvA.start(nameA);
    recvB.start(nameB);

    std::this_thread::sleep_for(Ms(100));

    EXPECT_EQ(logos_core_send_token(nameA.c_str(), tokenA.c_str()), 1);
    EXPECT_EQ(logos_core_send_token(nameB.c_str(), tokenB.c_str()), 1);

    ASSERT_TRUE(waitUntil([&]() {
        return recvA.finished.load() && recvB.finished.load();
    }, 5000));

    recvA.join();
    recvB.join();

    EXPECT_EQ(recvA.token, tokenA);
    EXPECT_EQ(recvB.token, tokenB)
        << "token for B must not leak into receiver A";
}

// =============================================================================
// Instance-ID scoping — the token socket name is scoped by LOGOS_INSTANCE_ID
// so parallel Logos instances (e.g. two logoscore daemons, or Basecamp + a
// daemon) loading the same module name don't race on a shared socket.
// =============================================================================

namespace {

// Use the shared helper that the production code uses, so tests that
// reason about the socket file path stay correct when $TMPDIR is unset
// (e.g. on macOS under `nix run`, where Qt resolves
// confstr(_CS_DARWIN_USER_TEMP_DIR) and naive getenv("TMPDIR") would
// fall back to /tmp/ — a path the receiver never actually binds at).
std::string tmpDir() {
    return ::logos::qtCompatibleTempDir();
}

struct ScopedEnv {
    std::string name;
    bool hadPrev;
    std::string prev;
    ScopedEnv(const char* n, const char* v) : name(n) {
        const char* p = std::getenv(n);
        hadPrev = (p != nullptr);
        if (hadPrev) prev = p;
        if (v) setenv(n, v, 1); else unsetenv(n);
    }
    ~ScopedEnv() {
        if (hadPrev) setenv(name.c_str(), prev.c_str(), 1);
        else unsetenv(name.c_str());
    }
};

} // namespace

TEST_F(TokenExchangeTest, InstanceIdScoping_SocketNameIncludesInstanceId) {
    const std::string pluginName = "rt_inst_scoped";
    const std::string token = "11111111-aaaa-bbbb-cccc-222222222222";
    const std::string instanceId = "inst_test_abcdef";

    ScopedEnv env("LOGOS_INSTANCE_ID", instanceId.c_str());

    ReceiverHandle receiver;
    receiver.start(pluginName);

    logos_core_register_process(pluginName.c_str());

    const std::string scopedPath =
        tmpDir() + "/logos_token_" + pluginName + "_" + instanceId;
    const std::string unscopedPath =
        tmpDir() + "/logos_token_" + pluginName;

    ASSERT_TRUE(waitUntil([&]() { return std::filesystem::exists(scopedPath); }, 5000))
        << "receiver must bind on instance-scoped socket path: " << scopedPath;
    EXPECT_FALSE(std::filesystem::exists(unscopedPath))
        << "receiver must NOT bind on the un-scoped path when LOGOS_INSTANCE_ID is set";

    ASSERT_EQ(logos_core_send_token(pluginName.c_str(), token.c_str()), 1);
    ASSERT_TRUE(waitUntil([&]() { return receiver.finished.load(); }, 5000));
    receiver.join();

    EXPECT_EQ(receiver.token, token);
}

// A stale un-scoped socket file (e.g. left by an older binary without
// instance-ID scoping) must not block a new instance-scoped receiver.
TEST_F(TokenExchangeTest, InstanceIdScoping_StaleUnscopedSocketDoesNotBlockScopedReceiver) {
    const std::string pluginName = "rt_inst_stale";
    const std::string token = "cafef00d-aaaa-bbbb-cccc-000000000002";
    const std::string instanceId = "inst_test_stalecheck";

    // Drop a stale file at the un-scoped path. It must not interfere.
    // RAII-removed at scope exit so a failing ASSERT here doesn't leak the
    // stale file into subsequent test runs.
    const std::string unscopedPath =
        tmpDir() + "/logos_token_" + pluginName;
    { std::ofstream f(unscopedPath); f << "stale"; }
    ASSERT_TRUE(std::filesystem::exists(unscopedPath));
    struct FileGuard {
        std::string path;
        ~FileGuard() { std::error_code ec; std::filesystem::remove(path, ec); }
    } unscopedGuard{unscopedPath};

    ScopedEnv env("LOGOS_INSTANCE_ID", instanceId.c_str());

    ReceiverHandle receiver;
    receiver.start(pluginName);

    logos_core_register_process(pluginName.c_str());
    std::this_thread::sleep_for(Ms(100));

    ASSERT_EQ(logos_core_send_token(pluginName.c_str(), token.c_str()), 1);
    ASSERT_TRUE(waitUntil([&]() { return receiver.finished.load(); }, 5000));
    receiver.join();

    EXPECT_EQ(receiver.token, token);
}

// =============================================================================
// $TMPDIR unset — sender and receiver must agree on the socket path even
// when the env var is missing.
//
// This pins the regression behind PR #129: under `nix run` on macOS the
// parent inherited an empty TMPDIR, naive getenv-based code fell back to
// "/tmp/<name>" while Qt's QLocalServer::listen resolved
// "/var/folders/.../T/<name>" via confstr(_CS_DARWIN_USER_TEMP_DIR), and
// every module's token-handoff timed out. Both sides now route through
// `::logos::unixSocketPath`, so this round-trip succeeds whether
// TMPDIR is set, unset, or empty.
// =============================================================================

TEST_F(TokenExchangeTest, RoundTrip_SucceedsWithTmpdirUnset) {
    const std::string moduleName = "rt_no_tmpdir";
    const std::string token = "33333333-4444-5555-6666-777777777777";

    // Unset both env vars for the duration of the test:
    //   - TMPDIR is the regression we're pinning.
    //   - LOGOS_INSTANCE_ID may have been set by earlier tests via
    //     LogosInstance::id() (qputenv); when set, it suffixes the
    //     socket name. Unsetting both keeps the expected path simple.
    ScopedEnv tmpdirEnv("TMPDIR", nullptr);
    ScopedEnv instanceEnv("LOGOS_INSTANCE_ID", nullptr);

    // Both sides must agree on a non-/tmp path on macOS, where
    // confstr(_CS_DARWIN_USER_TEMP_DIR) returns the per-user temp dir
    // even with no TMPDIR set. On Linux the helper falls back to /tmp,
    // which is also fine — the only thing the test cares about is that
    // sender and receiver land on the same path.
    const std::string expectedSocket =
        ::logos::unixSocketPath("logos_token_" + moduleName);

    ReceiverHandle receiver;
    receiver.start(moduleName);

    logos_core_register_process(moduleName.c_str());

    ASSERT_TRUE(waitUntil(
        [&]() { return std::filesystem::exists(expectedSocket); }, 5000))
        << "receiver must bind at the helper-resolved path: "
        << expectedSocket;

    ASSERT_EQ(logos_core_send_token(moduleName.c_str(), token.c_str()), 1);
    ASSERT_TRUE(waitUntil([&]() { return receiver.finished.load(); }, 5000));
    receiver.join();

    EXPECT_EQ(receiver.token, token);
}

// =============================================================================
// F-011 — attacker-controlled module name must not flow unsanitized into
// QLocalServer::removeServer()/listen(), which would give an arbitrary
// file-unlink/clobber primitive.
//
// The module name originates from untrusted plugin JSON metadata and is
// propagated verbatim to the child host, which builds
//   socketName = "logos_token_" + moduleName
// and feeds ::logos::unixSocketPath(socketName) (= tempdir + "/" + name, no
// validation) to QLocalServer::removeServer() — which UNLINKS the resolved
// path — and then listen(). A name containing "/" and "../" escapes the temp
// dir, so removeServer() deletes an attacker-chosen file.
//
// The receiver (SubprocessTokenReceiver::receive, exposed here via
// logos_core_receive_auth_token) must reject any module name that is not a
// single safe path segment BEFORE deriving the socket path.
// =============================================================================

// A crafted module name with path-traversal components must NOT cause the
// receiver to delete a file outside the flat socket namespace.
//
// Exploit mechanics (local-attacker threat model). The receiver builds
//   socketName = "logos_token_" + moduleName            // glued, no separator
//   path       = tempDir + "/" + socketName
// and passes `path` to QLocalServer::removeServer(), which unlink()s it.
// POSIX `..` resolution requires every intermediate component to be an
// existing, searchable directory, so a *bare* leading "../" in the name does
// not traverse (it yields the literal component "logos_token_.."). But the
// temp dir is world-writable (or per-user), so a local attacker can first
// create the intermediate directory  tempDir/logos_token_<seg>  and then get
// the daemon to load a module named  "<seg>/../<victim>" . The path then
// resolves through that real directory straight to an attacker-chosen victim
// outside the socket namespace, and removeServer() deletes it with the host
// process's privileges.
//
// With the guard in place the receiver rejects the name (it is not a single
// path segment) before deriving any path, so the victim survives.
TEST_F(TokenExchangeTest, MaliciousName_DoesNotUnlinkFileOutsideSocketNamespace) {
    const std::string tmp = ::logos::qtCompatibleTempDir();
    const std::string tag = std::to_string(static_cast<long long>(::getpid()));

    // (1) The intermediate directory a local attacker pre-creates in the
    //     world-writable temp dir. Its name is "logos_token_" + <seg> so that
    //     the receiver's glued prefix lands inside a directory that exists.
    const std::string seg = "f011seg" + tag;
    const std::string intermediateDir = tmp + "/logos_token_" + seg;
    std::filesystem::create_directories(intermediateDir);

    // (2) The victim file, in a sibling dir reached by traversing back out of
    //     the intermediate dir. This stands in for any file the host can write
    //     (a config, a key, another module's socket, ...).
    const std::string victimDir = tmp + "/f011_victim_" + tag;
    std::filesystem::create_directories(victimDir);
    const std::string victimPath = victimDir + "/precious.dat";
    { std::ofstream f(victimPath); f << "do not delete me"; }

    struct DirGuard {
        std::string a, b;
        ~DirGuard() {
            std::error_code ec;
            std::filesystem::remove_all(a, ec);
            std::filesystem::remove_all(b, ec);
        }
    } guard{intermediateDir, victimDir};

    ASSERT_TRUE(std::filesystem::exists(victimPath));

    // moduleName = "<seg>/../f011_victim_<tag>/precious.dat"
    //  -> socket  = tmp/logos_token_<seg>/../f011_victim_<tag>/precious.dat
    //  -> resolves to tmp/f011_victim_<tag>/precious.dat (the victim)
    const std::string maliciousName =
        seg + "/../f011_victim_" + tag + "/precious.dat";

    // Pin the exploit construction: without the fix, the name resolves to
    // exactly the victim path, and every intermediate component exists, so
    // removeServer()'s unlink() would succeed.
    const std::string resolved =
        std::filesystem::path(
            ::logos::unixSocketPath("logos_token_" + maliciousName))
            .lexically_normal()
            .string();
    ASSERT_EQ(resolved, victimPath)
        << "test construction error: crafted name must resolve to the victim path";

    char* raw = logos_core_receive_auth_token(maliciousName.c_str());
    std::string token = raw ? std::string(raw) : std::string();
    delete[] raw;

    EXPECT_TRUE(token.empty())
        << "receiver must reject an unsafe module name (no token issued)";
    EXPECT_TRUE(std::filesystem::exists(victimPath))
        << "victim file must NOT be unlinked by removeServer() — path "
           "traversal in the module name escaped the socket namespace";
}

// The same primitive via LOGOS_INSTANCE_ID. The receiver appends the env var
// verbatim to the socket name (socketName += "_" + instanceId), so it too
// becomes a path segment and must be validated. Here the module name is benign
// but the instance ID carries the traversal. Same world-writable-tempdir setup:
// the attacker pre-creates the intermediate dir so the "../" resolves, and
// removeServer() would unlink the victim. With the guard the receiver rejects
// the instance ID before deriving any path, so the victim survives.
TEST_F(TokenExchangeTest, MaliciousInstanceId_DoesNotUnlinkFileOutsideSocketNamespace) {
    const std::string tmp = ::logos::qtCompatibleTempDir();
    const std::string tag = std::to_string(static_cast<long long>(::getpid()));

    const std::string moduleName = "benign_mod" + tag;
    const std::string seg = "f011iseg" + tag;

    // socketName = "logos_token_" + moduleName + "_" + instanceId, so the
    // intermediate directory that must exist for the traversal is
    //   tmp/logos_token_<moduleName>_<seg>
    const std::string intermediateDir =
        tmp + "/logos_token_" + moduleName + "_" + seg;
    std::filesystem::create_directories(intermediateDir);

    const std::string victimDir = tmp + "/f011_ivictim_" + tag;
    std::filesystem::create_directories(victimDir);
    const std::string victimPath = victimDir + "/precious.dat";
    { std::ofstream f(victimPath); f << "do not delete me"; }

    struct DirGuard {
        std::string a, b;
        ~DirGuard() {
            std::error_code ec;
            std::filesystem::remove_all(a, ec);
            std::filesystem::remove_all(b, ec);
        }
    } guard{intermediateDir, victimDir};

    ASSERT_TRUE(std::filesystem::exists(victimPath));

    // instanceId = "<seg>/../f011_ivictim_<tag>/precious.dat"
    const std::string maliciousInstanceId =
        seg + "/../f011_ivictim_" + tag + "/precious.dat";

    // Pin the exploit construction: the resulting socket path resolves to the
    // victim, and every intermediate component exists.
    const std::string resolved =
        std::filesystem::path(
            ::logos::unixSocketPath(
                "logos_token_" + moduleName + "_" + maliciousInstanceId))
            .lexically_normal()
            .string();
    ASSERT_EQ(resolved, victimPath)
        << "test construction error: crafted instance ID must resolve to the victim path";

    ScopedEnv env("LOGOS_INSTANCE_ID", maliciousInstanceId.c_str());

    char* raw = logos_core_receive_auth_token(moduleName.c_str());
    std::string token = raw ? std::string(raw) : std::string();
    delete[] raw;

    EXPECT_TRUE(token.empty())
        << "receiver must reject an unsafe LOGOS_INSTANCE_ID (no token issued)";
    EXPECT_TRUE(std::filesystem::exists(victimPath))
        << "victim file must NOT be unlinked by removeServer() — path "
           "traversal in LOGOS_INSTANCE_ID escaped the socket namespace";
}

// The shared guard itself: pin the exact policy so the token-socket sink and
// the instance-persistence sink stay in agreement on what a safe name is.
TEST_F(TokenExchangeTest, IsSafePathSegment_RejectsTraversalAndSeparators) {
    using ::logos::isSafePathSegment;

    // Accept ordinary module names.
    EXPECT_TRUE(isSafePathSegment("chat_module"));
    EXPECT_TRUE(isSafePathSegment("waku-module.v2"));
    EXPECT_TRUE(isSafePathSegment(std::string(255, 'a')));

    // Reject anything that is not a single safe path segment.
    EXPECT_FALSE(isSafePathSegment(""));
    EXPECT_FALSE(isSafePathSegment("."));
    EXPECT_FALSE(isSafePathSegment(".."));
    EXPECT_FALSE(isSafePathSegment("a/b"));
    EXPECT_FALSE(isSafePathSegment("../escape"));
    EXPECT_FALSE(isSafePathSegment("a\\b"));
    EXPECT_FALSE(isSafePathSegment(std::string("nul\0byte", 8)));
    EXPECT_FALSE(isSafePathSegment(std::string(256, 'a')));
}

// =============================================================================
// F-012 — the token-handoff socket must not be reachable by other local users.
//
// Exploit scenario this guards against: on a shared/multi-user host, an
// attacker under a different uid knows a module (say "chat_module") is about to
// load — module names aren't secret — and races the parent by repeatedly
// connect()ing to the predictable socket path /tmp/logos_token_chat_module.
// If it wins the connection, it writes a token T it chose; the child accepts T
// verbatim as its auth credential. The attacker then presents T on inter-module
// calls and is authorized as that module, bypassing the capability/token gate
// (CWE-862, missing peer authentication on a local IPC channel).
//
// Root cause: the child listened with QLocalServer's default NoOptions, so the
// socket node was created at that public path under the world-writable $TMPDIR
// with mode 0777 & ~umask (≈0755) — group/other can connect.
//
// The fix sets QLocalServer::UserAccessOption so the node is owner-only
// (0600), which stops a different-uid attacker from connecting at all. We can't
// spawn a second uid in CI to run the inject directly, but that socket mode is
// the exact precondition that makes it possible — so we assert the node is
// owner-only. Before the fix this test fails (group/other bits set); after it,
// the node carries no group/other access.
// =============================================================================

TEST_F(TokenExchangeTest, TokenSocket_IsNotAccessibleByOtherUsers) {
    const std::string moduleName = "rt_sock_perms";
    const std::string token = "5ec0ff12-0000-0000-0000-000000000012";

    // Pin the path: unset both env vars that influence socket naming so the
    // expected path is simple and deterministic (see RoundTrip_..TmpdirUnset).
    ScopedEnv tmpdirEnv("TMPDIR", nullptr);
    ScopedEnv instanceEnv("LOGOS_INSTANCE_ID", nullptr);

    const std::string socketPath =
        ::logos::unixSocketPath("logos_token_" + moduleName);

    // Start a receiver and wait for it to bind the socket node, then inspect
    // its mode while it is still listening.
    ReceiverHandle receiver;
    receiver.start(moduleName);
    logos_core_register_process(moduleName.c_str());

    ASSERT_TRUE(waitUntil(
        [&]() { return std::filesystem::exists(socketPath); }, 5000))
        << "receiver never bound the token socket at: " << socketPath;

    struct stat st{};
    ASSERT_EQ(::stat(socketPath.c_str(), &st), 0)
        << "stat() failed for token socket: " << socketPath;

    // The vulnerability is group/other being able to reach the socket. With
    // UserAccessOption the node is owner-only; assert no group/other bits.
    // Before the fix the default NoOptions left the node at 0777 & ~umask
    // (≈0755 with umask 022), so this expectation failed.
    EXPECT_EQ(st.st_mode & (S_IRWXG | S_IRWXO), 0u)
        << "token socket is reachable by group/other (mode "
        << std::oct << (st.st_mode & 07777) << std::dec
        << "); a co-tenant uid could inject an auth token (F-012)";

    // The legitimate same-uid handoff must still succeed with the socket
    // hardened and the peer-uid check in place — also unblocks the receiver
    // so the test finishes immediately instead of waiting out the 10s timeout.
    ASSERT_EQ(logos_core_send_token(moduleName.c_str(), token.c_str()), 1)
        << "hardened socket must not break the legitimate same-uid handoff";
    ASSERT_TRUE(waitUntil([&]() { return receiver.finished.load(); }, 5000));
    receiver.join();
    EXPECT_EQ(receiver.token, token);
}

// =============================================================================
// Peer-credential helper — the primitive behind the sender/receiver peer
// authentication. Both sides call ::logos::socketPeerIsSameUid() to refuse a
// peer running under a different uid. We can't fabricate a different-uid peer
// in CI, but we can pin the two contractual behaviours the hardening relies on:
//   - a same-uid connected peer is accepted (so the legitimate handoff works)
//   - a bad fd fails closed (so errors never read as "trusted")
// =============================================================================

TEST_F(TokenExchangeTest, PeerCredentials_SameUidSocketPairIsTrusted) {
    int sv[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0)
        << "socketpair() failed: " << std::strerror(errno);

    // Both ends belong to this process → same uid → must be trusted.
    EXPECT_TRUE(::logos::socketPeerIsSameUid(sv[0]));
    EXPECT_TRUE(::logos::socketPeerIsSameUid(sv[1]));

    ::close(sv[0]);
    ::close(sv[1]);
}

TEST_F(TokenExchangeTest, PeerCredentials_BadFdFailsClosed) {
    // A negative/closed fd must be rejected — the helper fails closed so a
    // syscall error can never be mistaken for a trusted peer.
    EXPECT_FALSE(::logos::socketPeerIsSameUid(-1));
}
