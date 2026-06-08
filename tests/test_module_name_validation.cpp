// =============================================================================
// Regression tests for F-030 — path traversal / token-handoff redirection via
// an unvalidated module name (CWE-22).
//
// The attack: a malicious installed module declares name='../<x>' (or a name
// that collides with another module's socket). The name comes verbatim from
// untrusted plugin metadata and is used as a single path segment for the
// per-module auth-token handoff socket:
//
//     "logos_token_" + name  ->  qtCompatibleTempDir() + "/" + name
//
// Before the fix the only guard was a sun_path length check, so '/' or '..' in
// the name escaped the temp dir and moved where the parent connect()s to hand
// off the token and where the child QLocalServer binds. A local attacker who
// pre-binds that resolved path captures the per-module auth token the parent
// writes — and that token grants the module's full RPC privileges.
//
// These tests pin the contract introduced by the fix:
//   1. logos::isValidModuleName allow-lists safe identifiers and rejects
//      every separator / traversal / control sequence.
//   2. The sender (SubprocessContainer::sendTokenToProcess) refuses to hand
//      the token to a socket derived from an unsafe name — even when an
//      attacker is already listening at the resolved path.
//   3. The child receiver (SubprocessTokenReceiver::receive) refuses to bind
//      a socket for an unsafe name (it returns empty instead of escaping the
//      temp dir or hanging for its full timeout).
// =============================================================================
#include <gtest/gtest.h>
#include "qt_test_adapter.h"
#include "unix_socket_path.h"
#include "module_name_validation.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QString>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
using Ms    = std::chrono::milliseconds;

// Restore the previous value of an env var on scope exit so one test can't
// perturb the socket-path scheme seen by another.
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

// The exact filesystem path the production code derives for a module's
// token-handoff socket. LOGOS_INSTANCE_ID is unset by the fixtures below so
// no instance suffix is appended.
std::string tokenSocketPath(const std::string& moduleName) {
    return ::logos::unixSocketPath("logos_token_" + moduleName);
}

} // namespace

class ModuleNameValidationTest : public ::testing::Test {
protected:
    void SetUp() override { logos_core_clear_processes(); }
    void TearDown() override { logos_core_clear_processes(); }
};

// =============================================================================
// Validator unit tests — the single source of truth shared by the registry
// trust boundary and both socket sinks.
// =============================================================================

TEST_F(ModuleNameValidationTest, Validator_AcceptsRealModuleNames) {
    // Names that ship today in the workspace's metadata.json files.
    EXPECT_TRUE(::logos::isValidModuleName("chat"));
    EXPECT_TRUE(::logos::isValidModuleName("chat_module"));
    EXPECT_TRUE(::logos::isValidModuleName("waku_module"));
    EXPECT_TRUE(::logos::isValidModuleName("accounts_ui"));
    EXPECT_TRUE(::logos::isValidModuleName("package-downloader"));
    EXPECT_TRUE(::logos::isValidModuleName("Counter123"));
}

TEST_F(ModuleNameValidationTest, Validator_RejectsPathSeparatorsAndTraversal) {
    EXPECT_FALSE(::logos::isValidModuleName("../evil"));
    EXPECT_FALSE(::logos::isValidModuleName("../../etc/passwd"));
    EXPECT_FALSE(::logos::isValidModuleName("a/b"));
    EXPECT_FALSE(::logos::isValidModuleName("foo\\bar"));
    EXPECT_FALSE(::logos::isValidModuleName(".."));
    EXPECT_FALSE(::logos::isValidModuleName("."));
}

TEST_F(ModuleNameValidationTest, Validator_RejectsEmptyControlAndOverlong) {
    EXPECT_FALSE(::logos::isValidModuleName(""));
    EXPECT_FALSE(::logos::isValidModuleName(std::string("evil\0hidden", 10)));  // embedded NUL
    EXPECT_FALSE(::logos::isValidModuleName("has space"));
    EXPECT_FALSE(::logos::isValidModuleName("dot.name"));     // '.' is not in the allowlist
    EXPECT_FALSE(::logos::isValidModuleName(std::string(65, 'a')));  // > 64 bytes
    EXPECT_TRUE(::logos::isValidModuleName(std::string(64, 'a')));   // boundary: 64 ok
}

// =============================================================================
// Sender side — token-handoff redirection / interception.
//
// This is the core exploit: an attacker pre-binds a socket at the path that
// an unsafe module name resolves to and waits to capture the per-module auth
// token the parent writes. The sender must refuse to deliver the token for a
// name that is not a safe identifier.
//
// `evil.pwn` stands in for any non-allow-listed name: it contains no '/', so
// pre-fix it flowed straight into a bindable temp-dir path and the parent
// happily connected and wrote the token there. Post-fix the sender rejects it
// before computing any path. (Traversal names with '/' and '..' are pinned by
// the validator tests above.)
// =============================================================================

TEST_F(ModuleNameValidationTest, Sender_RefusesTokenHandoffForUnsafeName_NoInterception) {
    ScopedEnv instanceEnv("LOGOS_INSTANCE_ID", nullptr);

    const std::string maliciousName = "evil.pwn";
    const std::string token = "5ec4e7-token-must-not-leak-0001";
    const std::string attackerPath = tokenSocketPath(maliciousName);
    const QString attackerPathQ = QString::fromStdString(attackerPath);

    // Attacker process pre-binds the resolved path and tries to read whatever
    // the parent hands off. Runs in its own thread so the (blocking) accept
    // happens while the sender executes on the main thread.
    std::atomic<bool> listening{false};
    std::string captured;
    std::thread attacker([&]() {
        QLocalServer::removeServer(attackerPathQ);
        QLocalServer server;
        if (!server.listen(attackerPathQ)) return;
        listening = true;
        if (server.waitForNewConnection(1500)) {
            QLocalSocket* c = server.nextPendingConnection();
            if (c && c->waitForReadyRead(1000))
                captured = QString::fromUtf8(c->readAll()).toStdString();
        }
        server.close();
    });

    // Wait until the attacker is actually listening before sending.
    auto deadline = Clock::now() + Ms(2000);
    while (!listening.load() && Clock::now() < deadline)
        std::this_thread::sleep_for(Ms(10));
    ASSERT_TRUE(listening.load()) << "attacker socket failed to bind at " << attackerPath;

    const int sent = logos_core_send_token(maliciousName.c_str(), token.c_str());

    attacker.join();
    std::error_code ec;
    std::filesystem::remove(attackerPath, ec);

    EXPECT_EQ(sent, 0)
        << "sendToken must refuse an unsafe module name instead of connecting "
           "to the attacker-controlled socket path";
    EXPECT_TRUE(captured.empty())
        << "auth token was handed off to the attacker socket: '" << captured << "'";
}

// A safe name still round-trips through send → an attacker-free receiver, so
// the validation guard hasn't broken the legitimate handoff path. (Full
// happy-path coverage lives in test_token_exchange.cpp; this is a focused
// positive control alongside the negative cases above.)
TEST_F(ModuleNameValidationTest, Sender_RefusesWhenNobodyListening_ButValidNameIsAccepted) {
    ScopedEnv instanceEnv("LOGOS_INSTANCE_ID", nullptr);

    // Valid name, nobody listening: the send fails on connect (not on
    // validation) after its retry budget. A short budget keeps the test fast
    // while still proving a valid name is *accepted* by the guard and reaches
    // the connect stage.
    const std::string validName = "valid_module";
    auto start = Clock::now();
    const int sent = SubprocessManager::sendTokenToProcess(validName, "tok", /*max_wait_ms=*/200);
    auto elapsed = std::chrono::duration_cast<Ms>(Clock::now() - start).count();

    EXPECT_EQ(sent, 0) << "no server is listening, so the handoff cannot succeed";
    EXPECT_GE(elapsed, 150)
        << "a valid name must pass validation and spend its connect-retry "
           "budget, not be rejected up front";
}

// =============================================================================
// Receiver side — the child must not bind a socket for an unsafe name.
//
// Pre-fix, the child computed the same unsanitised path and called
// QLocalServer::listen() on it, then blocked for its full 10s timeout. Post-fix
// it short-circuits: returns empty immediately and creates no socket file.
// =============================================================================

TEST_F(ModuleNameValidationTest, Receiver_RefusesToBindForUnsafeName) {
    ScopedEnv instanceEnv("LOGOS_INSTANCE_ID", nullptr);

    const std::string maliciousName = "evil.pwn";
    const std::string socketPath = tokenSocketPath(maliciousName);

    std::error_code ec;
    std::filesystem::remove(socketPath, ec);  // start clean

    auto start = Clock::now();
    char* raw = logos_core_receive_auth_token(maliciousName.c_str());
    auto elapsed = std::chrono::duration_cast<Ms>(Clock::now() - start).count();

    std::string token = raw ? raw : "";
    delete[] raw;

    EXPECT_TRUE(token.empty())
        << "receiver must not produce a token for an unsafe module name";
    EXPECT_FALSE(std::filesystem::exists(socketPath))
        << "receiver bound a socket for an unsafe name at " << socketPath;
    EXPECT_LT(elapsed, 2000)
        << "receiver must reject the unsafe name up front, not block on its "
           "full listen timeout (got " << elapsed << "ms)";

    std::filesystem::remove(socketPath, ec);  // defensive cleanup
}

TEST_F(ModuleNameValidationTest, Receiver_RefusesTraversalName) {
    ScopedEnv instanceEnv("LOGOS_INSTANCE_ID", nullptr);

    // A genuine traversal payload. The receiver must reject it outright.
    const std::string traversalName = "../../tmp/logos_pwned";

    char* raw = logos_core_receive_auth_token(traversalName.c_str());
    std::string token = raw ? raw : "";
    delete[] raw;

    EXPECT_TRUE(token.empty())
        << "receiver must refuse a traversal module name";
    // The naive resolved path must not have been created anywhere.
    EXPECT_FALSE(std::filesystem::exists(tokenSocketPath(traversalName)));
}
