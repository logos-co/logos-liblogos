// What core sends to other modules, and how it reaches them.

#include "fake_module_host.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

class CoreOutboundTest : public FakeHostFixture {
protected:
    void SetUp() override {
        FakeHostFixture::SetUp();
        useFakeHost();
    }

    void TearDown() override {
        logos_core_set_access_policy(nullptr);
        FakeHostFixture::TearDown();
    }

    // Known from its metadata sidecar, as an installed module is.
    void installModule(const std::string& name, const std::vector<std::string>& dependencies) {
        const fs::path binary = tmp.path / (name + "_plugin.so");
        std::ofstream(binary) << "report-ok\n";
        std::ofstream(tmp.path / (name + "_plugin.metadata.json"))
            << nlohmann::json{{"name", name}, {"version", "1.0.0"},
                              {"dependencies", dependencies}}.dump();
        char* registered = logos_core_process_module(binary.string().c_str());
        ASSERT_NE(registered, nullptr);
        delete[] registered;
    }
};

} // namespace

// tcp was removed in logos-protocol 0.15. A module configured with it fails
// to load, with why, rather than serving its local socket alone.
TEST_F(CoreOutboundTest, AModuleConfiguredWithARemovedTransportIsRefused)
{
    logos_core_set_module_transports("churn",
                                     R"([{"protocol":"tcp","host":"127.0.0.1","port":6001}])");
    plantModule("churn", "report-ok");
    EXPECT_EQ(logos_core_load_module("churn", LOGOS_LOAD_MODULE_ONLY), 0);
    EXPECT_FALSE(logos_core_is_module_loaded("churn"));
    const std::string reason = reasonFor("churn", logos::module_state::kError);
    EXPECT_NE(reason.find("removed in protocol 0.15"), std::string::npos) << reason;
    logos_core_set_module_transports("churn", "");
}

// Detector: restriction pushes ran on each loading thread, so under
// concurrent loads a policy read before a dependent committed could reach
// capability_module last and refuse that declared caller.
TEST_F(CoreOutboundTest, TheLastRestrictionPushedIsTheLatest)
{
    logos_core_set_access_policy(R"({"version":1,"mode":"enforce","restrictions":{}})");
    installModule("target_a", {});
    installModule("caller_x", {"target_a"});
    installModule("caller_y", {"target_a"});
    ASSERT_EQ(logos_core_load_module("target_a", LOGOS_LOAD_MODULE_ONLY), 1);

    // X's document for A is slow to land; Y loads while it is in flight.
    std::mutex mutex;
    std::condition_variable changed;
    bool slowEntered = false;
    stand_in::onRestrictions([&](const nlohmann::json& document) {
        const auto callers = document.value("target_a", nlohmann::json::array());
        const auto names = [&](const char* who) {
            return std::find(callers.begin(), callers.end(), who) != callers.end();
        };
        if (!names("caller_x") || names("caller_y")) return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            slowEntered = true;
        }
        changed.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    });
    std::thread loadX([] {
        EXPECT_EQ(logos_core_load_module("caller_x", LOGOS_LOAD_MODULE_ONLY), 1);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds(20), [&] { return slowEntered; }));
    }
    EXPECT_EQ(logos_core_load_module("caller_y", LOGOS_LOAD_MODULE_ONLY), 1);
    loadX.join();
    stand_in::onRestrictions({});

    const auto documents = stand_in::restrictionDocuments();
    ASSERT_FALSE(documents.empty());
    const auto callers =
        nlohmann::json::parse(documents.back()).value("target_a", nlohmann::json::array());
    for (const char* caller : {"caller_x", "caller_y"})
        EXPECT_NE(std::find(callers.begin(), callers.end(), caller), callers.end())
            << caller << " is not an allowed caller of target_a: " << callers.dump();
    for (const char* name : {"caller_x", "caller_y", "target_a"})
        logos_core_unload_module(name, false);
}
