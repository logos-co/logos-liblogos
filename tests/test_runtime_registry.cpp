// =============================================================================
// Tests for RuntimeRegistry: registration, selection, fan-out operations.
//
// These tests run entirely in-process with FakeRuntime stubs — no Qt event
// loop, no subprocess, no file I/O.
// =============================================================================
#include <gtest/gtest.h>
#include "runtime_registry.h"
#include <string>
#include <unordered_map>
#include <vector>

using namespace LogosCore;

// ---------------------------------------------------------------------------
// A minimal in-process stub runtime used by every test below.
// Anonymous namespace prevents ODR conflicts with other FakeRuntime definitions
// in the same binary (e.g. test_module_runtime_abstraction.cpp).
// ---------------------------------------------------------------------------
namespace {

struct FakeRuntime : public ModuleRuntime {
    explicit FakeRuntime(std::string myId, std::string handledFormat = "")
        : m_id(std::move(myId)), m_handledFormat(std::move(handledFormat)) {}

    std::string id() const override { return m_id; }

    bool canHandle(const ModuleDescriptor& desc) const override {
        if (m_handledFormat.empty()) return true; // accepts anything
        return desc.format == m_handledFormat;
    }

    bool load(const ModuleDescriptor& desc,
              std::function<void(const std::string&)>,
              LoadedModuleHandle& out) override {
        out.name = desc.name;
        out.pid  = 42;
        loadCalls.push_back(desc.name);
        return loadShouldSucceed;
    }

    bool sendToken(const std::string& name, const std::string& token) override {
        sendTokenCalls.push_back({name, token});
        return true;
    }

    void terminate(const std::string& name) override {
        terminateCalls.push_back(name);
    }

    void terminateAll() override { terminateAllCount++; }

    bool hasModule(const std::string& name) const override {
        (void)name; return false;
    }

    std::unordered_map<std::string, int64_t> getAllPids() const override {
        return fakePids;
    }

    // Controllable behaviour
    bool loadShouldSucceed = true;

    // Call records
    std::vector<std::string>                             loadCalls;
    std::vector<std::pair<std::string, std::string>>     sendTokenCalls;
    std::vector<std::string>                             terminateCalls;
    int                                                  terminateAllCount = 0;

    // Configurable pids for getAllPids()
    std::unordered_map<std::string, int64_t>             fakePids;

private:
    std::string m_id;
    std::string m_handledFormat;
};

} // anonymous namespace

// =============================================================================
// Empty registry
// =============================================================================

TEST(RuntimeRegistryTest, SelectReturnsNullWhenEmpty) {
    RuntimeRegistry reg;
    ModuleDescriptor desc;
    desc.name   = "foo";
    desc.format = "qt-plugin";
    EXPECT_EQ(reg.select(desc), nullptr);
}

TEST(RuntimeRegistryTest, TerminateAllOnEmptyRegistryDoesNotCrash) {
    RuntimeRegistry reg;
    EXPECT_NO_THROW(reg.terminateAll());
}

TEST(RuntimeRegistryTest, GetAllPidsOnEmptyRegistryReturnsEmptyMap) {
    RuntimeRegistry reg;
    EXPECT_TRUE(reg.getAllPids().empty());
}

// =============================================================================
// canHandle dispatch
// =============================================================================

TEST(RuntimeRegistryTest, SelectPicksFirstCanHandleRuntime) {
    RuntimeRegistry reg;
    auto rtA = std::make_shared<FakeRuntime>("a", "qt-plugin");
    auto rtB = std::make_shared<FakeRuntime>("b", "qt-plugin");
    reg.registerRuntime(rtA);
    reg.registerRuntime(rtB);

    ModuleDescriptor desc;
    desc.format = "qt-plugin";
    auto selected = reg.select(desc);
    ASSERT_NE(selected, nullptr);
    EXPECT_EQ(selected->id(), "a");
}

TEST(RuntimeRegistryTest, SelectSkipsRuntimeThatCannotHandle) {
    RuntimeRegistry reg;
    auto rtA = std::make_shared<FakeRuntime>("a", "wasm");
    auto rtB = std::make_shared<FakeRuntime>("b", "qt-plugin");
    reg.registerRuntime(rtA);
    reg.registerRuntime(rtB);

    ModuleDescriptor desc;
    desc.format = "qt-plugin";
    auto selected = reg.select(desc);
    ASSERT_NE(selected, nullptr);
    EXPECT_EQ(selected->id(), "b");
}

TEST(RuntimeRegistryTest, SelectReturnsNullIfNoRuntimeHandlesFormat) {
    RuntimeRegistry reg;
    auto rtA = std::make_shared<FakeRuntime>("a", "wasm");
    reg.registerRuntime(rtA);

    ModuleDescriptor desc;
    desc.format = "qt-plugin";
    EXPECT_EQ(reg.select(desc), nullptr);
}

// =============================================================================
// Explicit runtimeConfig["id"] override
// =============================================================================

TEST(RuntimeRegistryTest, SelectUsesExplicitIdOverride) {
    RuntimeRegistry reg;
    auto rtA = std::make_shared<FakeRuntime>("a", "qt-plugin");
    auto rtB = std::make_shared<FakeRuntime>("b", "qt-plugin");
    reg.registerRuntime(rtA);
    reg.registerRuntime(rtB);

    ModuleDescriptor desc;
    desc.format = "qt-plugin";
    desc.runtimeConfig["id"] = "b"; // explicitly request the second one

    auto selected = reg.select(desc);
    ASSERT_NE(selected, nullptr);
    EXPECT_EQ(selected->id(), "b");
}

TEST(RuntimeRegistryTest, SelectReturnsNullForUnknownExplicitId) {
    RuntimeRegistry reg;
    auto rtA = std::make_shared<FakeRuntime>("a", "qt-plugin");
    reg.registerRuntime(rtA);

    ModuleDescriptor desc;
    desc.runtimeConfig["id"] = "nonexistent-id";
    EXPECT_EQ(reg.select(desc), nullptr);
}

TEST(RuntimeRegistryTest, ExplicitIdDoesNotFallThroughToCanHandle) {
    // Even if "other" is the only runtime that canHandle, requesting "missing"
    // explicitly must return nullptr rather than silently routing to "other".
    RuntimeRegistry reg;
    auto rtA = std::make_shared<FakeRuntime>("other"); // accepts anything
    reg.registerRuntime(rtA);

    ModuleDescriptor desc;
    desc.runtimeConfig["id"] = "missing";
    EXPECT_EQ(reg.select(desc), nullptr);
}

// =============================================================================
// terminateAll fan-out
// =============================================================================

TEST(RuntimeRegistryTest, TerminateAllCallsEveryRuntime) {
    RuntimeRegistry reg;
    auto rtA = std::make_shared<FakeRuntime>("a");
    auto rtB = std::make_shared<FakeRuntime>("b");
    reg.registerRuntime(rtA);
    reg.registerRuntime(rtB);

    reg.terminateAll();

    EXPECT_EQ(rtA->terminateAllCount, 1);
    EXPECT_EQ(rtB->terminateAllCount, 1);
}

// =============================================================================
// getAllPids aggregation
// =============================================================================

TEST(RuntimeRegistryTest, GetAllPidsAggregatesAcrossRuntimes) {
    RuntimeRegistry reg;
    auto rtA = std::make_shared<FakeRuntime>("a");
    auto rtB = std::make_shared<FakeRuntime>("b");
    rtA->fakePids["mod1"] = 100;
    rtA->fakePids["mod2"] = 200;
    rtB->fakePids["mod3"] = 300;
    reg.registerRuntime(rtA);
    reg.registerRuntime(rtB);

    auto pids = reg.getAllPids();
    EXPECT_EQ(pids.size(), 3u);
    EXPECT_EQ(pids.at("mod1"), 100);
    EXPECT_EQ(pids.at("mod2"), 200);
    EXPECT_EQ(pids.at("mod3"), 300);
}

// =============================================================================
// clearForTests
// =============================================================================

TEST(RuntimeRegistryTest, ClearForTests_RemovesAllRuntimes) {
    RuntimeRegistry reg;
    reg.registerRuntime(std::make_shared<FakeRuntime>("a"));
    reg.registerRuntime(std::make_shared<FakeRuntime>("b"));

    reg.clearForTests();

    ModuleDescriptor desc;
    EXPECT_EQ(reg.select(desc), nullptr);
    EXPECT_NO_THROW(reg.terminateAll());
}
