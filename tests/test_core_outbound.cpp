// What core sends to other modules, and how it reaches them.

#include "fake_module_host.h"
#include "logos_protocol.h"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

char* copyOut(const std::string& value)
{
    auto* out = static_cast<char*>(std::malloc(value.size() + 1));
    std::memcpy(out, value.c_str(), value.size() + 1);
    return out;
}

char* noMethods(void*) { return copyOut("[]"); }

std::uint16_t freeTcpPort()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
    ::close(fd);
    return ntohs(address.sin_port);
}

struct Received {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> methods;
};

char* recordMethod(const char* method, const char*, void* userData)
{
    auto& received = *static_cast<Received*>(userData);
    {
        std::lock_guard<std::mutex> lock(received.mutex);
        received.methods.push_back(method);
    }
    received.changed.notify_all();
    return copyOut("true");
}

// A capability_module that keeps the last caller list it was given per target,
// and is slow to take one: A's list naming X but not Y.
struct Restrictions {
    std::mutex mutex;
    std::condition_variable changed;
    bool slowEntered = false;
    std::map<std::string, std::vector<std::string>> latest;
};

char* restrictionDispatch(const char* method, const char* argsJson, void* userData)
{
    auto& r = *static_cast<Restrictions*>(userData);
    const auto args = nlohmann::json::parse(argsJson, nullptr, false);
    if (std::strcmp(method, "registerRestriction") != 0 || !args.is_array() || args.size() != 3)
        return copyOut("true");
    const std::string target = args[1].get<std::string>();
    const auto callers = args[2].get<std::vector<std::string>>();
    const auto names = [&](const char* who) {
        return std::find(callers.begin(), callers.end(), who) != callers.end();
    };
    if (target == "target_a" && names("caller_x") && !names("caller_y")) {
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            r.slowEntered = true;
        }
        r.changed.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        r.latest[target] = callers;
    }
    r.changed.notify_all();
    return copyOut("true");
}

class CoreOutboundTest : public FakeHostFixture {
protected:
    void SetUp() override {
        FakeHostFixture::SetUp();
        useFakeHost();
    }

    void TearDown() override {
        logos_core_set_access_policy(nullptr);
        logos_core_set_module_transports("modules_state", "");
        FakeHostFixture::TearDown();
    }

    // Known from its metadata sidecar, as an installed module is.
    void installModule(const std::string& name, const std::vector<std::string>& dependencies) {
        const fs::path binary = tmp.path / (name + "_plugin.so");
        std::ofstream(binary) << "report-ok\n";
        std::ofstream(tmp.path / (name + "_plugin.metadata.json"))
            << nlohmann::json{{"name", name}, {"version", "1.0.0"},
                              {"dependencies", dependencies}}.dump();
        char* registered = logos_core_process_module(binary.c_str());
        ASSERT_NE(registered, nullptr);
        delete[] registered;
    }

    // A stand-in for a loaded module, served from this process.
    lp_provider* serve(const std::string& name, const char* transports, lp_dispatch_cb dispatch,
                       void* userData) {
        char* token = logos_core_get_token(name.c_str());
        EXPECT_NE(token, nullptr);
        lp_provider* provider = lp_provider_create(name.c_str(), transports);
        EXPECT_NE(provider, nullptr);
        EXPECT_EQ(lp_provider_save_token(provider, "core", token ? token : ""), LP_OK);
        delete[] token;
        EXPECT_EQ(lp_provider_register(provider, dispatch, noMethods, nullptr, userData), LP_OK);
        return provider;
    }
};

} // namespace

// Detector: core rewrote every module's tcp or tcp_ssl transport to local, so
// an embedder's module listening on tcp only was unreachable. Here the one
// core dials on its own: modules_state, for the lifecycle feed.
TEST_F(CoreOutboundTest, ATcpOnlyModuleIsReachedOverTcp)
{
    const std::string tcp = "[{\"protocol\":\"tcp\",\"host\":\"127.0.0.1\",\"port\":"
        + std::to_string(freeTcpPort()) + "}]";
    logos_core_set_module_transports("modules_state", tcp.c_str());
    plantModule("modules_state", "report-ok");
    ASSERT_EQ(logos_core_load_module("modules_state", LOGOS_LOAD_MODULE_ONLY), 1);
    Received received;
    lp_provider* stateModule = serve("modules_state", tcp.c_str(), recordMethod, &received);

    plantModule("churn", "report-ok");
    ASSERT_EQ(logos_core_load_module("churn", LOGOS_LOAD_MODULE_ONLY), 1);
    {
        std::unique_lock<std::mutex> lock(received.mutex);
        EXPECT_TRUE(received.changed.wait_for(lock, std::chrono::seconds(15), [&] {
            return std::count(received.methods.begin(), received.methods.end(),
                              "note_transition") > 0;
        })) << "nothing reached modules_state over tcp";
    }
    ASSERT_EQ(logos_core_unload_module("churn", false), 1);
    ASSERT_EQ(logos_core_unload_module("modules_state", false), 1);
    lp_provider_destroy(stateModule);
}

// Detector: restriction pushes ran on each loading thread, so under
// concurrent loads a caller list read before a dependent committed could reach
// capability_module last and refuse that declared caller.
TEST_F(CoreOutboundTest, TheLastRestrictionPushedIsTheLatest)
{
    logos_core_set_access_policy(R"({"version":1,"mode":"enforce","restrictions":{}})");
    plantModule("capability_module", "report-ok");
    ASSERT_EQ(logos_core_load_module("capability_module", LOGOS_LOAD_MODULE_ONLY), 1);
    Restrictions restrictions;
    lp_provider* capability = serve("capability_module", nullptr, restrictionDispatch,
                                    &restrictions);
    installModule("target_a", {});
    installModule("caller_x", {"target_a"});
    installModule("caller_y", {"target_a"});
    ASSERT_EQ(logos_core_load_module("target_a", LOGOS_LOAD_MODULE_ONLY), 1);

    // X's push for A is slow; Y loads while it is in flight.
    std::thread loadX([] {
        EXPECT_EQ(logos_core_load_module("caller_x", LOGOS_LOAD_MODULE_ONLY), 1);
    });
    {
        std::unique_lock<std::mutex> lock(restrictions.mutex);
        ASSERT_TRUE(restrictions.changed.wait_for(lock, std::chrono::seconds(20),
                                                  [&] { return restrictions.slowEntered; }));
    }
    EXPECT_EQ(logos_core_load_module("caller_y", LOGOS_LOAD_MODULE_ONLY), 1);
    loadX.join();

    std::vector<std::string> callers;
    {
        std::lock_guard<std::mutex> lock(restrictions.mutex);
        callers = restrictions.latest["target_a"];
    }
    for (const char* caller : {"caller_x", "caller_y"})
        EXPECT_NE(std::find(callers.begin(), callers.end(), caller), callers.end())
            << caller << " is not an allowed caller of target_a: "
            << nlohmann::json(callers).dump();
    for (const char* name : {"caller_x", "caller_y", "target_a", "capability_module"})
        logos_core_unload_module(name, false);
    lp_provider_destroy(capability);
}
