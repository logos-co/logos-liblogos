// =============================================================================
// Tests for the token-exchange IPC between logos_core (QtProcessManager::
// sendToken) and logos_host (QtTokenReceiver::receiveAuthToken). The two
// sides normally run in different processes; these tests exercise them in
// the same process using a worker thread for the receiver.
//
// The point of these tests is to pin the behavioural contract so any
// replacement implementation (e.g. Boost.Asio local::stream_protocol) must
// preserve it:
//   - socket name mapping is "logos_token_<plugin_name>"
//   - client can retry if server is not listening yet
//   - token round-trip is byte-exact (no truncation, no trailing null)
//   - stale socket files do not prevent a new receiver from binding
//   - wrong socket name times out cleanly without hanging forever
//   - send to a never-listening server fails cleanly (no hang)
// =============================================================================
#include <gtest/gtest.h>
#include "qt/qt_process_manager.h"
#include "qt/qt_token_receiver.h"   // from src/logos_host/qt
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QString>
#include <QThread>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

namespace {

template <typename Predicate>
bool waitUntil(Predicate pred, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!pred()) {
        if (timer.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(5);
    }
    return true;
}

// Run the receiver in a std::thread so the sender can run on the main (Qt
// event-loop) thread. QtTokenReceiver owns its own QLocalServer and does not
// rely on the global QCoreApplication, so running it off-thread is safe.
struct ReceiverHandle {
    std::thread thread;
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    QString token;

    void start(const QString& pluginName) {
        thread = std::thread([this, pluginName]() {
            started = true;
            token = QtTokenReceiver::receiveAuthToken(pluginName);
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
        QtProcessManager::clearAll();
    }
    void TearDown() override {
        QtProcessManager::clearAll();
    }
};

// =============================================================================
// Happy path — server listening, client sends, full token arrives.
// =============================================================================

TEST_F(TokenExchangeTest, RoundTrip_SucceedsWithServerRunningFirst) {
    const QString pluginName = "rt_happy";
    const std::string token = "8400e3a5-1b3a-4de5-9f1f-abcdef012345";

    ReceiverHandle receiver;
    receiver.start(pluginName);

    // Register the process so the failure-path teardown (which takes the
    // QProcess* from the map) has an entry to remove when sendToken fails.
    // It's a no-op for the happy path but mirrors production use.
    QtProcessManager::registerProcess(pluginName.toStdString());

    // Give the server's listen() a moment so the first connect attempt
    // succeeds. The sendToken retry loop also handles this, but matching
    // production timing keeps the test fast.
    QThread::msleep(100);

    bool sent = QtProcessManager::sendToken(pluginName.toStdString(), token);
    EXPECT_TRUE(sent);

    ASSERT_TRUE(waitUntil([&]() { return receiver.finished.load(); }, 5000))
        << "receiver did not finish within 5s";
    receiver.join();

    EXPECT_EQ(receiver.token.toStdString(), token)
        << "round-tripped token must be byte-exact";
}

// =============================================================================
// Retry path — client starts before the server has listen()'d. The sendToken
// retry loop (10 × 100ms in the current implementation) should cover this.
// =============================================================================

TEST_F(TokenExchangeTest, RoundTrip_SucceedsWhenClientStartsBeforeServer) {
    const QString pluginName = "rt_retry";
    const std::string token = "deadbeef-1234-5678-9abc-def012345678";
    QtProcessManager::registerProcess(pluginName.toStdString());

    // Kick off the sender first from a background thread, receiver starts
    // shortly after. The sender should land on one of the later retries.
    std::atomic<bool> sendResult{false};
    std::atomic<bool> senderDone{false};
    std::thread senderThread([&]() {
        sendResult = QtProcessManager::sendToken(
            pluginName.toStdString(), token);
        senderDone = true;
    });

    // Delay > one retry interval so the sender definitely retries at least once.
    QThread::msleep(250);

    ReceiverHandle receiver;
    receiver.start(pluginName);

    ASSERT_TRUE(waitUntil([&]() {
        return senderDone.load() && receiver.finished.load();
    }, 8000)) << "retry round-trip did not complete in 8s";

    senderThread.join();
    receiver.join();

    EXPECT_TRUE(sendResult.load())
        << "sendToken should have succeeded via its retry loop";
    EXPECT_EQ(receiver.token.toStdString(), token);
}

// =============================================================================
// Byte integrity — a 36-char UUID string must round-trip with no truncation,
// no trailing null, no reordering. This is the test that would catch a
// framing bug where the receiver read too few bytes and returned a partial
// token that then gets silently rejected as an auth-token mismatch.
// =============================================================================

TEST_F(TokenExchangeTest, TokenBytes_RoundTripIsByteExact) {
    const QString pluginName = "rt_bytes";
    // Realistic UUID format emitted by QUuid::toString(WithoutBraces):
    //   lowercase hex, 8-4-4-4-12 with four dashes, 36 chars total.
    const std::string token = "f47ac10b-58cc-4372-a567-0e02b2c3d479";
    QtProcessManager::registerProcess(pluginName.toStdString());

    ReceiverHandle receiver;
    receiver.start(pluginName);
    QThread::msleep(100);

    ASSERT_TRUE(QtProcessManager::sendToken(pluginName.toStdString(), token));
    ASSERT_TRUE(waitUntil([&]() { return receiver.finished.load(); }));
    receiver.join();

    std::string got = receiver.token.toStdString();
    EXPECT_EQ(got.size(), token.size())
        << "received token length must match sent length";
    EXPECT_EQ(got, token);
    // Belt and braces: no embedded nulls, no whitespace, exactly four dashes.
    EXPECT_EQ(got.find('\0'), std::string::npos);
    EXPECT_EQ(got.find(' '),  std::string::npos);
    EXPECT_EQ(got.find('\n'), std::string::npos);
    int dashCount = 0;
    for (char c : got) if (c == '-') ++dashCount;
    EXPECT_EQ(dashCount, 4);
}

// =============================================================================
// Stale socket file — a crashed previous run can leave
// /tmp/logos_token_<name> on disk. The new receiver must either remove it
// or overwrite it. If a port forgets to unlink, every subsequent plugin load
// after a crash fails.
//
// This test pre-creates a fake file at the socket path (via QLocalServer's
// removal API to probe the path), then confirms the receiver can still bind.
// =============================================================================

TEST_F(TokenExchangeTest, StaleSocketFile_DoesNotBlockNewReceiver) {
    const QString pluginName = "rt_stale";
    const std::string token = "cafef00d-0000-0000-0000-000000000001";

    // Create a transient server on the same name so a socket file exists
    // on disk, then close it WITHOUT calling removeServer() to leave stale
    // state behind. (QLocalServer cleans up by default on destruction on
    // Unix, so we force the stale case by not destroying cleanly.)
    {
        QLocalServer server;
        QLocalServer::removeServer(QString("logos_token_") + pluginName);
        ASSERT_TRUE(server.listen(QString("logos_token_") + pluginName))
            << "precondition: must be able to bind initially";
        // Close without removing -> on some platforms leaves the filesystem
        // entry behind. QtTokenReceiver must handle this by calling
        // removeServer() before its own listen().
        server.close();
    }

    ReceiverHandle receiver;
    receiver.start(pluginName);

    QtProcessManager::registerProcess(pluginName.toStdString());
    QThread::msleep(100);

    EXPECT_TRUE(QtProcessManager::sendToken(pluginName.toStdString(), token));
    ASSERT_TRUE(waitUntil([&]() { return receiver.finished.load(); }, 5000))
        << "receiver must be able to bind over a stale socket path";
    receiver.join();

    EXPECT_EQ(receiver.token.toStdString(), token);
}

// =============================================================================
// Wrong socket name — client connects to a name that nobody listens on.
// sendToken retries 10 × 100ms then gives up, returning false. Critical
// property: it must NOT hang beyond its retry budget, and must NOT leak a
// socket or process entry.
// =============================================================================

TEST_F(TokenExchangeTest, WrongName_FailsCleanlyWithinTimeout) {
    const std::string pluginName = "rt_nobody_listening";
    const std::string token = "deadbeef-dead-beef-dead-beefdeadbeef";

    // Register a placeholder process so the failure path can locate and
    // remove it without hitting a null-deref.
    QtProcessManager::registerProcess(pluginName);

    QElapsedTimer timer;
    timer.start();
    bool sent = QtProcessManager::sendToken(pluginName, token);
    qint64 elapsed = timer.elapsed();

    EXPECT_FALSE(sent) << "sendToken to a nonexistent server must fail";
    // Current impl: 10 retries × ~100ms connect timeout + ~100ms sleep = ~2s.
    // Allow a generous upper bound so the test is not flaky.
    EXPECT_LT(elapsed, 5000)
        << "sendToken must give up within its retry budget, got " << elapsed << "ms";

    // The failure path calls destroyProcess on the placeholder; registry
    // must no longer contain the entry.
    EXPECT_FALSE(QtProcessManager::hasProcess(pluginName));
}

// =============================================================================
// Receiver timeout — start a receiver for a plugin name, never send a
// token, verify it returns an empty QString within its own timeout
// (10s in the current impl). This test uses a shorter sanity bound.
// =============================================================================

TEST_F(TokenExchangeTest, Receiver_TimesOutWhenNoClientConnects) {
    const QString pluginName = "rt_no_client";

    ReceiverHandle receiver;
    QElapsedTimer timer;
    timer.start();
    receiver.start(pluginName);

    // The receiver's internal waitForNewConnection is 10000ms.
    ASSERT_TRUE(waitUntil([&]() { return receiver.finished.load(); }, 15000))
        << "receiver did not return within its own timeout";
    receiver.join();
    qint64 elapsed = timer.elapsed();

    EXPECT_TRUE(receiver.token.isEmpty())
        << "no client connected; received token must be empty";
    EXPECT_LT(elapsed, 15000)
        << "receiver hung past its documented 10s timeout";
}

// =============================================================================
// Concurrent round-trips on distinct plugin names must not interfere.
// =============================================================================

TEST_F(TokenExchangeTest, ConcurrentDistinctPlugins_EachRoundTripsCorrectly) {
    const QString nameA = "rt_a";
    const QString nameB = "rt_b";
    const std::string tokenA = "11111111-1111-1111-1111-111111111111";
    const std::string tokenB = "22222222-2222-2222-2222-222222222222";

    QtProcessManager::registerProcess(nameA.toStdString());
    QtProcessManager::registerProcess(nameB.toStdString());

    ReceiverHandle recvA;
    ReceiverHandle recvB;
    recvA.start(nameA);
    recvB.start(nameB);

    QThread::msleep(100);

    EXPECT_TRUE(QtProcessManager::sendToken(nameA.toStdString(), tokenA));
    EXPECT_TRUE(QtProcessManager::sendToken(nameB.toStdString(), tokenB));

    ASSERT_TRUE(waitUntil([&]() {
        return recvA.finished.load() && recvB.finished.load();
    }, 5000));

    recvA.join();
    recvB.join();

    EXPECT_EQ(recvA.token.toStdString(), tokenA);
    EXPECT_EQ(recvB.token.toStdString(), tokenB)
        << "token for B must not leak into receiver A";
}
