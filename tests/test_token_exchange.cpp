// =============================================================================
// Tests for the token-exchange IPC between logos_core (send) and
// receive (logos_core_receive_auth_token). The two sides normally run in
// different processes; these tests exercise them in the same process using a
// worker thread for the receiver.
//
// The point of these tests is to pin the behavioural contract so any
// replacement implementation must preserve it:
//   - socket name mapping is "logos_token_<plugin_name>"
//   - client can retry if server is not listening yet
//   - token round-trip is byte-exact (no truncation, no trailing null)
//   - stale socket files do not prevent a new receiver from binding
//   - wrong socket name times out cleanly without hanging forever
//   - send to a never-listening server fails cleanly (no hang)
// =============================================================================
#include <gtest/gtest.h>
#include "logos_core.h"
#include "qt_test_adapter.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>

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

    void start(const std::string& pluginName) {
        thread = std::thread([this, pluginName]() {
            started = true;
            char* raw = logos_core_receive_auth_token(pluginName.c_str());
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
    const std::string pluginName = "rt_happy";
    const std::string token = "8400e3a5-1b3a-4de5-9f1f-abcdef012345";

    ReceiverHandle receiver;
    receiver.start(pluginName);

    logos_core_register_process(pluginName.c_str());

    // Give the server's listen() a moment so the first connect attempt succeeds.
    std::this_thread::sleep_for(Ms(100));

    int sent = logos_core_send_token(pluginName.c_str(), token.c_str());
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
    const std::string pluginName = "rt_retry";
    const std::string token = "deadbeef-1234-5678-9abc-def012345678";
    logos_core_register_process(pluginName.c_str());

    std::atomic<int> sendResult{0};
    std::atomic<bool> senderDone{false};
    std::thread senderThread([&]() {
        sendResult = logos_core_send_token(pluginName.c_str(), token.c_str());
        senderDone = true;
    });

    // Delay > one retry interval so the sender definitely retries at least once.
    std::this_thread::sleep_for(Ms(250));

    ReceiverHandle receiver;
    receiver.start(pluginName);

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
    const std::string pluginName = "rt_bytes";
    const std::string token = "f47ac10b-58cc-4372-a567-0e02b2c3d479";
    logos_core_register_process(pluginName.c_str());

    ReceiverHandle receiver;
    receiver.start(pluginName);
    std::this_thread::sleep_for(Ms(100));

    ASSERT_EQ(logos_core_send_token(pluginName.c_str(), token.c_str()), 1);
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
    const std::string pluginName = "rt_stale";
    const std::string token = "cafef00d-0000-0000-0000-000000000001";

    // Create and immediately close a socket file to simulate stale state.
    logos_core_create_stale_token_socket(pluginName.c_str());

    ReceiverHandle receiver;
    receiver.start(pluginName);

    logos_core_register_process(pluginName.c_str());
    std::this_thread::sleep_for(Ms(100));

    EXPECT_EQ(logos_core_send_token(pluginName.c_str(), token.c_str()), 1);
    ASSERT_TRUE(waitUntil([&]() { return receiver.finished.load(); }, 5000))
        << "receiver must be able to bind over a stale socket path";
    receiver.join();

    EXPECT_EQ(receiver.token, token);
}

// =============================================================================
// Wrong socket name — client connects to a name nobody listens on.
// =============================================================================

TEST_F(TokenExchangeTest, WrongName_FailsCleanlyWithinTimeout) {
    const std::string pluginName = "rt_nobody_listening";
    const std::string token = "deadbeef-dead-beef-dead-beefdeadbeef";

    logos_core_register_process(pluginName.c_str());

    auto start = Clock::now();
    int sent = logos_core_send_token(pluginName.c_str(), token.c_str());
    auto elapsed = std::chrono::duration_cast<Ms>(Clock::now() - start).count();

    EXPECT_EQ(sent, 0) << "sendToken to a nonexistent server must fail";
    EXPECT_LT(elapsed, 5000)
        << "sendToken must give up within its retry budget, got " << elapsed << "ms";

    // The failure path removes the placeholder entry.
    EXPECT_EQ(logos_core_has_process(pluginName.c_str()), 0);
}

// =============================================================================
// Receiver timeout — start a receiver for a plugin name, never send a token.
// =============================================================================

TEST_F(TokenExchangeTest, Receiver_TimesOutWhenNoClientConnects) {
    const std::string pluginName = "rt_no_client";

    ReceiverHandle receiver;
    auto start = Clock::now();
    receiver.start(pluginName);

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
// Concurrent round-trips on distinct plugin names must not interfere.
// =============================================================================

TEST_F(TokenExchangeTest, ConcurrentDistinctPlugins_EachRoundTripsCorrectly) {
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

std::string tmpDir() {
    const char* tmp = std::getenv("TMPDIR");
    std::string dir = (tmp && tmp[0]) ? tmp : "/tmp";
    while (!dir.empty() && dir.back() == '/') dir.pop_back();
    return dir;
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
    std::this_thread::sleep_for(Ms(100));

    const std::string scopedPath =
        tmpDir() + "/logos_token_" + pluginName + "_" + instanceId;
    const std::string unscopedPath =
        tmpDir() + "/logos_token_" + pluginName;

    EXPECT_TRUE(std::filesystem::exists(scopedPath))
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
    const std::string unscopedPath =
        tmpDir() + "/logos_token_" + pluginName;
    { std::ofstream f(unscopedPath); f << "stale"; }
    ASSERT_TRUE(std::filesystem::exists(unscopedPath));

    ScopedEnv env("LOGOS_INSTANCE_ID", instanceId.c_str());

    ReceiverHandle receiver;
    receiver.start(pluginName);

    logos_core_register_process(pluginName.c_str());
    std::this_thread::sleep_for(Ms(100));

    ASSERT_EQ(logos_core_send_token(pluginName.c_str(), token.c_str()), 1);
    ASSERT_TRUE(waitUntil([&]() { return receiver.finished.load(); }, 5000));
    receiver.join();

    EXPECT_EQ(receiver.token, token);

    // Cleanup the stale file we dropped.
    std::filesystem::remove(unscopedPath);
}
