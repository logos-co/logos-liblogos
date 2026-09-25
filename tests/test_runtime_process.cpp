// The runtime in a process of its own (bin/logos_runtime), spawned from this
// test process, which then plays the app: it reaches the runtime only through
// module calls, as its shell, and serves the hooks the runtime forwards.
#include <gtest/gtest.h>
#include "logos_core.h"
#include "logging/logos_log.h"
#include "module_manager.h"
#include "qt_test_adapter.h"
#include "test_platform.h"
#include "logos_protocol.h"

#include <nlohmann/json.hpp>
#include <spdlog/sinks/base_sink.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

using json = nlohmann::json;

namespace {

template <typename Predicate>
bool eventually(Predicate predicate, std::chrono::seconds limit = std::chrono::seconds(15))
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return predicate();
}

// A zombie counts as gone: nothing runs in it.
bool alive(std::int64_t pid)
{
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process) return false;
    DWORD code = 0;
    const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
#else
    if (::kill(static_cast<pid_t>(pid), 0) != 0) return false;
#ifdef __APPLE__
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
    struct kinfo_proc info {};
    size_t size = sizeof info;
    if (sysctl(mib, 4, &info, &size, nullptr, 0) == 0 && size > 0)
        return info.kp_proc.p_stat != SZOMB;
    return true;
#else
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string text((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    const auto end = text.rfind(')');
    return end == std::string::npos || end + 2 >= text.size() || text[end + 2] != 'Z';
#endif
#endif
}

json callAs(logos_consumer* binding, const char* method, const json& args = json::array())
{
    char* result = nullptr;
    char* error = nullptr;
    const int status = logos_consumer_call(binding, "core_service", method, args.dump().c_str(),
                                           15000, &result, &error);
    json value = status == LP_OK && result ? json::parse(result, nullptr, false) : json(nullptr);
    logos_consumer_string_free(result);
    logos_consumer_string_free(error);
    return value;
}

// A client that presents `token` to core_service, as logosctl does.
json callWithToken(const std::string& identity, const std::string& token, const char* method,
                   const json& args = json::array())
{
    lp_token_isolate_identity(identity.c_str());
    lp_token_save_for(identity.c_str(), "core_service", token.c_str());
    lp_client* client = lp_client_create("core_service", identity.c_str(), nullptr, nullptr);
    if (!client) return nullptr;
    char* result = nullptr;
    char* error = nullptr;
    const int status = lp_invoke(client, method, args.dump().c_str(), 15000, &result, &error);
    json value = status == LP_OK && result ? json::parse(result, nullptr, false)
                                           : json{{"failed", error ? error : ""}};
    lp_string_free(result);
    lp_string_free(error);
    lp_client_destroy(client);
    return value;
}

std::int64_t pidOf(const json& stats, const std::string& module)
{
    if (!stats.is_array()) return 0;
    for (const auto& entry : stats)
        if (entry.is_object() && entry.value("name", std::string{}) == module)
            return entry.value("pid", std::int64_t{0});
    return 0;
}

// What the runtime's log relay writes here, for as long as a case records it.
class RecordingSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::string text()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return m_text;
    }
    void record(bool on)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        m_recording = on;
        if (on) m_text.clear();
    }

protected:
    void sink_it_(const spdlog::details::log_msg& message) override
    {
        if (!m_recording) return;
        m_text.append(message.payload.data(), message.payload.size());
        m_text.push_back('\n');
    }
    void flush_() override {}

private:
    std::string m_text;
    bool m_recording = false;
};

// Added once and never removed: the relay may still be writing when a case ends.
RecordingSink& relayRecorder()
{
    static const std::shared_ptr<RecordingSink> sink = [] {
        auto recorder = std::make_shared<RecordingSink>();
        logos::logger("runtime").sinks().push_back(recorder);
        return recorder;
    }();
    return *sink;
}

std::atomic<int> g_shutdowns{0};

char* testOperators(const char* token, const char*, void*)
{
    return token && std::string(token) == "alice-token" ? lp_string_copy("alice") : nullptr;
}

void testShutdown(void*)
{
    ++g_shutdowns;
}

// The embedder's own method: answers with the caller the runtime named.
char* testExtension(const char* callerJson, const char* method, const char*, void*)
{
    return method && std::string(method) == "whoAmI" ? lp_string_copy(callerJson) : nullptr;
}

constexpr const char* kExtensionMethods =
    R"([{"type":"method","name":"whoAmI","returnType":"LogosMap","isInvokable":true,"parameters":[]}])";

// The runtime a case spawned, stopped however the case ends.
struct Spawned {
    logos_runtime* runtime = nullptr;

    Spawned() = default;
    Spawned(const Spawned&) = delete;
    Spawned& operator=(const Spawned&) = delete;
    ~Spawned() { stop(); }

    void stop()
    {
        logos_runtime_stop(runtime);
        runtime = nullptr;
    }
};

// An environment variable put back as it was when the case ends.
struct SavedEnv {
    std::string name;
    std::optional<std::string> value;

    explicit SavedEnv(const char* variable) : name(variable)
    {
        if (const char* current = std::getenv(variable)) value = current;
    }
    ~SavedEnv()
    {
        if (value) logos_test::setEnv(name.c_str(), *value);
        else logos_test::unsetEnv(name.c_str());
    }
};

} // namespace

class RuntimeProcessTest : public ::testing::Test {
protected:
    std::string bundled;
    SavedEnv instance{"LOGOS_INSTANCE_ID"};
    SavedEnv host{"LOGOS_HOST_PATH"};

    void SetUp() override
    {
        logos_core_terminate_all();
        logos_core_clear();
        const char* dir = std::getenv("TEST_BUNDLED_MODULES_DIR");
        if (!dir || !*dir) {
            if (std::getenv("LOGOS_REQUIRE_TEST_FIXTURES")) FAIL() << "TEST_BUNDLED_MODULES_DIR not set";
            GTEST_SKIP() << "TEST_BUNDLED_MODULES_DIR not set";
        }
        bundled = dir;
        // The real hosts, whatever an earlier case left in LOGOS_HOST_PATH.
        if (const char* real = std::getenv("TEST_REAL_HOST"); real && *real)
            logos_test::setEnv("LOGOS_HOST_PATH", real);
        // Sockets of their own: nothing an earlier case left here shares them.
        static int cases = 0;
        char id[13];
        std::snprintf(id, sizeof id, "%06llx%06x",
                      static_cast<unsigned long long>(logos_test::currentPid()) & 0xffffffULL, ++cases);
        logos_test::setEnv("LOGOS_INSTANCE_ID", id);
    }

    void TearDown() override
    {
        logos_core_clear();
    }

    json config(const json& extra = json::object()) const
    {
        json value = {{"shell", "basecamp"}, {"bundled_modules_dirs", json::array({bundled})}};
        for (const auto& [key, item] : extra.items()) value[key] = item;
        return value;
    }
};

TEST_F(RuntimeProcessTest, TheShellReachesItsRuntimeOnlyThroughModuleCalls)
{
    ASSERT_EQ(logos_core_set_operator_resolver(&testOperators, nullptr), 0);
    ASSERT_EQ(logos_core_set_shutdown_handler(&testShutdown, nullptr), 0);
    ASSERT_EQ(logos_core_set_core_service_extension(&testExtension, kExtensionMethods, nullptr), 0);
    g_shutdowns = 0;
    RecordingSink& relay = relayRecorder();
    relay.record(true);

    char* error = nullptr;
    const json placement = {{"modules", {{"modules_state", "subprocess"}}}};
    Spawned spawned;
    logos_runtime* runtime = spawned.runtime =
        logos_runtime_spawn(config({{"placement_policy", placement}}).dump().c_str(), &error);
    ASSERT_NE(runtime, nullptr) << (error ? error : "");
    logos_consumer* shell = logos_runtime_binding(runtime);
    ASSERT_NE(shell, nullptr);
    EXPECT_STREQ(logos_consumer_name(shell), "basecamp");
    EXPECT_FALSE(ModuleManager::started()) << "nothing runs in this process";

    // Another process, and the token authority lives there.
    const json status = callAs(shell, "getStatus");
    ASSERT_TRUE(status.is_object()) << status.dump();
    const std::int64_t runtimePid = status["daemon"].value("pid", std::int64_t{0});
    EXPECT_GT(runtimePid, 0);
    EXPECT_NE(runtimePid, logos_test::currentPid());
    const json authority = callAs(shell, "getModuleInfo", {"capability_module"});
    EXPECT_EQ(authority.value("placement", std::string{}), "inproc") << authority.dump();

    // Shell scope reaches across the socket: it admits a presentation consumer.
    const json admitted = callAs(shell, "admitConsumer", {"runtime_test_view", "presentation"});
    EXPECT_EQ(admitted.value("status", std::string{}), "ok") << admitted.dump();
    EXPECT_FALSE(admitted.value("credential", std::string{}).empty());

    // A module placed in a subprocess runs as the runtime's child.
    std::int64_t hostPid = 0;
    ASSERT_TRUE(eventually([&] { return (hostPid = pidOf(callAs(shell, "getModuleStats"), "modules_state")) > 0; }));
    EXPECT_EQ(callAs(shell, "getModuleInfo", {"modules_state"}).value("placement", std::string{}),
              "subprocess");

    // The embedder's own method is answered here, for the caller the runtime named.
    EXPECT_EQ(callAs(shell, "whoAmI"), (json{{"kind", "module"}, {"name", "basecamp"}}));

    // An operator's token is named here too, and a stranger's is not.
    const json asAlice = callWithToken("@test-alice", "alice-token", "listModules", {"all"});
    EXPECT_TRUE(asAlice.is_array()) << asAlice.dump();
    const json asMallory = callWithToken("@test-mallory", "mallory-token", "listModules", {"all"});
    EXPECT_FALSE(asMallory.is_array()) << asMallory.dump();

    // core_service.shutdown reaches the app's handler.
    const json shutdown = callWithToken("@test-alice", "alice-token", "shutdown");
    EXPECT_EQ(shutdown.value("status", std::string{}), "ok") << shutdown.dump();
    EXPECT_TRUE(eventually([] { return g_shutdowns.load() == 1; }));

    // A module file the app names is processed there, and only there.
    if (const char* fixture = std::getenv("TEST_PLUGIN_DEP_RANGE"); fixture && *fixture) {
        char* name = logos_runtime_process_module(runtime, fixture);
        EXPECT_STREQ(name ? name : "", "dep_range_fixture");
        logos_consumer_string_free(name);
        EXPECT_NE(callAs(shell, "listModules", {"all"}).dump().find("dep_range_fixture"),
                  std::string::npos);
    }

    char* raw = logos_consumer_credential(shell);
    const std::string credential = raw ? raw : "";
    logos_consumer_string_free(raw);
    ASSERT_FALSE(credential.empty());

    // A stop unloads its modules and ends it, and its hosts with it.
    spawned.stop();
    EXPECT_FALSE(alive(runtimePid));
    EXPECT_TRUE(eventually([&] { return !alive(hostPid); })) << "modules_state's host outlived it";

    relay.record(false);
    const std::string log = relay.text();
    EXPECT_NE(log.find("logos_runtime is ready"), std::string::npos) << "its log never reached here";
    EXPECT_EQ(log.find(credential), std::string::npos) << "its log carried the shell's credential";
}

TEST_F(RuntimeProcessTest, WithoutItsAuthorityItNeverBecomesReady)
{
    // The same modules, not bundled: capability_module cannot run in-process.
    char* error = nullptr;
    const json unbundled = {{"shell", "basecamp"}, {"modules_dirs", json::array({bundled})}};
    Spawned failed;
    failed.runtime = logos_runtime_spawn(unbundled.dump().c_str(), &error);
    EXPECT_EQ(failed.runtime, nullptr);
    ASSERT_NE(error, nullptr);
    EXPECT_NE(std::string(error).find("no token authority"), std::string::npos) << error;
    logos_consumer_string_free(error);

    // A failed spawn leaves room for the next one.
    error = nullptr;
    Spawned next;
    next.runtime = logos_runtime_spawn(config().dump().c_str(), &error);
    EXPECT_NE(next.runtime, nullptr) << (error ? error : "");
}

TEST_F(RuntimeProcessTest, OneRuntimePerProcess)
{
    char* error = nullptr;
    Spawned spawned;
    spawned.runtime = logos_runtime_spawn(config().dump().c_str(), &error);
    ASSERT_NE(spawned.runtime, nullptr) << (error ? error : "");

    char* second = nullptr;
    Spawned another;
    another.runtime = logos_runtime_spawn(config().dump().c_str(), &second);
    EXPECT_EQ(another.runtime, nullptr);
    EXPECT_NE(std::string(second ? second : "").find("already"), std::string::npos);
    logos_consumer_string_free(second);
    // Nor may it start one of its own beside it.
    logos_core_start();
    EXPECT_FALSE(ModuleManager::started());

    char* noShell = nullptr;
    spawned.stop();
    another.runtime = logos_runtime_spawn(R"({"bundled_modules_dirs":[]})", &noShell);
    EXPECT_EQ(another.runtime, nullptr);
    EXPECT_NE(std::string(noShell ? noShell : "").find("shell"), std::string::npos);
    logos_consumer_string_free(noShell);
}

TEST_F(RuntimeProcessTest, AnUnexpectedExitIsReported)
{
    char* error = nullptr;
    Spawned spawned;
    logos_runtime* runtime = spawned.runtime = logos_runtime_spawn(config().dump().c_str(), &error);
    ASSERT_NE(runtime, nullptr) << (error ? error : "");
    const std::int64_t pid =
        callAs(logos_runtime_binding(runtime), "getStatus")["daemon"].value("pid", std::int64_t{0});
    ASSERT_GT(pid, 0);

    struct Seen {
        std::mutex mutex;
        std::vector<std::string> reasons;
    } seen;
    logos_runtime_on_exit(
        runtime,
        [](const char* reason, void* data) {
            auto& s = *static_cast<Seen*>(data);
            std::lock_guard<std::mutex> lock(s.mutex);
            s.reasons.emplace_back(reason ? reason : "");
        },
        &seen);
    ASSERT_TRUE(logos_test::killPid(pid));
    EXPECT_TRUE(eventually([&] {
        std::lock_guard<std::mutex> lock(seen.mutex);
        return !seen.reasons.empty();
    }));
    {
        std::lock_guard<std::mutex> lock(seen.mutex);
        ASSERT_EQ(seen.reasons.size(), 1u);
        EXPECT_NE(seen.reasons.front().find("runtime"), std::string::npos) << seen.reasons.front();
    }
    EXPECT_EQ(logos_runtime_process_module(runtime, "/nowhere"), nullptr);
    spawned.stop();
    std::lock_guard<std::mutex> lock(seen.mutex);
    EXPECT_EQ(seen.reasons.size(), 1u) << "a stop is no second exit";
}

// SIGKILL on the app (TerminateProcess on Windows) takes the runtime and its hosts down.
TEST_F(RuntimeProcessTest, TheRuntimeAndItsHostsGoDownWithTheirApp)
{
    const auto app = logos_test::thisExecutable().parent_path()
#ifdef _WIN32
        / "logos_runtime_test_app.exe";
#else
        / "logos_runtime_test_app";
#endif
    ASSERT_TRUE(std::filesystem::exists(app)) << app;
    const json placement = {{"modules", {{"modules_state", "subprocess"}}}};
    logos_test::Child child;
    ASSERT_TRUE(child.start(app.string(), {config({{"placement_policy", placement}}).dump()}, true));

    std::int64_t runtimePid = 0;
    std::vector<std::int64_t> hosts;
    std::string output;
    char buffer[512];
    while (output.find("READY\n") == std::string::npos && output.find("SPAWN_FAILED") == std::string::npos) {
        const size_t n = child.read(buffer, sizeof buffer);
        if (n == 0) break;
        output.append(buffer, n);
    }
    ASSERT_NE(output.find("READY\n"), std::string::npos) << output;
    std::istringstream lines(output);
    for (std::string word; lines >> word;) {
        std::int64_t pid = 0;
        if (word == "RUNTIME_PID" && lines >> pid) runtimePid = pid;
        else if (word == "HOST_PID" && lines >> pid) hosts.push_back(pid);
    }
    ASSERT_GT(runtimePid, 0) << output;
    ASSERT_FALSE(hosts.empty()) << output;

    child.kill();
    EXPECT_TRUE(eventually([&] { return !alive(runtimePid); })) << "the runtime outlived its app";
    for (const auto host : hosts)
        EXPECT_TRUE(eventually([&] { return !alive(host); })) << "host " << host << " outlived its app";
}
