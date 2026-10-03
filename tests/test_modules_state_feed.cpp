// The lifecycle feed to modules_state: every transition arrives, in order.

#include "fake_module_host.h"
#include "logos_protocol.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Transition {
    long long seq;
    std::string module;
    std::string state;
};

struct FeedLog {
    std::mutex mutex;
    std::vector<Transition> arrived;   // note_transition calls, in arrival order
};

char* copyOut(const std::string& value)
{
    auto* out = static_cast<char*>(std::malloc(value.size() + 1));
    std::memcpy(out, value.c_str(), value.size() + 1);
    return out;
}

char* feedDispatch(const char* method, const char* argsJson, void* userData)
{
    auto& log = *static_cast<FeedLog*>(userData);
    const auto args = nlohmann::json::parse(argsJson, nullptr, false);
    if (std::strcmp(method, "note_transition") == 0 && args.is_array() && args.size() == 7) {
        std::lock_guard<std::mutex> lock(log.mutex);
        log.arrived.push_back({args[6].get<long long>(), args[0].get<std::string>(),
                               args[4].get<std::string>()});
    }
    return copyOut("true");
}

char* feedMethods(void*) { return copyOut("[]"); }

// Until nothing new has arrived for a second.
void waitForQuiet(FeedLog& log)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::size_t seen = 0;
    auto quietSince = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline
           && std::chrono::steady_clock::now() - quietSince < std::chrono::seconds(1)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard<std::mutex> lock(log.mutex);
        if (log.arrived.size() != seen) {
            seen = log.arrived.size();
            quietSince = std::chrono::steady_clock::now();
        }
    }
}

class ModulesStateFeedTest : public FakeHostFixture {
protected:
    void SetUp() override {
        FakeHostFixture::SetUp();
        useFakeHost();
    }
};

} // namespace

// Detector: each transition went out on a thread of its own, so they reached
// modules_state out of order and its replay rule dropped the late ones.
TEST_F(ModulesStateFeedTest, TransitionsReachModulesStateInOrder)
{
    plantModule("modules_state", "report-ok");
    ASSERT_EQ(logos_core_load_module("modules_state", LOGOS_LOAD_MODULE_ONLY), 1);
    char* token = logos_core_get_token("modules_state");
    ASSERT_NE(token, nullptr);
    // A stand-in modules_state, served from this process on its endpoint.
    FeedLog log;
    lp_provider* stateModule = lp_provider_create("modules_state", nullptr);
    ASSERT_NE(stateModule, nullptr);
    ASSERT_EQ(lp_provider_save_token(stateModule, "core", token), LP_OK);
    delete[] token;
    ASSERT_EQ(lp_provider_register(stateModule, feedDispatch, feedMethods, nullptr, &log),
              LP_OK);

    plantModule("churn", "report-ok");
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(logos_core_load_module("churn", LOGOS_LOAD_MODULE_ONLY), 1);
        ASSERT_EQ(logos_core_unload_module("churn", false), 1);
    }
    waitForQuiet(log);
    ASSERT_EQ(logos_core_unload_module("modules_state", false), 1);
    waitForQuiet(log);
    lp_provider_destroy(stateModule);

    // modules_state drops a transition older than one it has seen, and a
    // snapshot takes a seq of its own, so: increasing, and none missing.
    std::lock_guard<std::mutex> lock(log.mutex);
    for (std::size_t i = 1; i < log.arrived.size(); ++i)
        EXPECT_GT(log.arrived[i].seq, log.arrived[i - 1].seq) << "at arrival " << i;
    int loaded = 0;
    int unloaded = 0;
    for (const Transition& t : log.arrived) {
        if (t.module != "churn") continue;
        loaded += t.state == logos::module_state::kLoaded;
        unloaded += t.state == logos::module_state::kUnloaded;
    }
    EXPECT_EQ(loaded, 10);
    EXPECT_EQ(unloaded, 10);
}
