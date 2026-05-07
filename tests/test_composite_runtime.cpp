// =============================================================================
// Tests for CompositeRuntime — verifies that it correctly delegates to its
// ModuleContainer and ModuleLoader, producing a valid ModuleRuntime.
//
// Uses fake/stub implementations to test the composition logic in isolation.
// No real processes are spawned.
// =============================================================================
#include <gtest/gtest.h>
#include "composite_runtime.h"
#include "module_container.h"
#include "module_loader.h"
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace LogosCore;

// ---------------------------------------------------------------------------
// Stubs — defined outside anonymous namespace to avoid Clang
// std::make_shared / compressed_pair issues with internal-linkage types.
// ---------------------------------------------------------------------------

struct FakeContainer : public ModuleContainer {
    std::string id() const override { return "fake-container"; }
    bool canHandle(const ModuleDescriptor&) const override { return containerCanHandle; }

    bool launch(const ModuleDescriptor& desc,
                const std::string& hostBinary,
                const std::vector<std::string>& args,
                std::function<void(const std::string&)>,
                LoadedModuleHandle& out) override {
        launchCalls.push_back({desc.name, hostBinary, args});
        if (!launchShouldSucceed) return false;
        out.name = desc.name;
        out.pid  = 42;
        activeModules.insert(desc.name);
        return true;
    }

    bool sendToken(const std::string& name, const std::string& token) override {
        sendTokenCalls.push_back({name, token});
        return sendTokenResult;
    }

    void terminate(const std::string& name) override {
        terminateCalls.push_back(name);
        activeModules.erase(name);
    }

    void terminateAll() override {
        terminateAllCount++;
        activeModules.clear();
    }

    bool hasModule(const std::string& name) const override {
        return activeModules.count(name) > 0;
    }

    std::optional<int64_t> pid(const std::string& name) const override {
        if (activeModules.count(name)) return 42;
        return std::nullopt;
    }

    std::unordered_map<std::string, int64_t> getAllPids() const override {
        std::unordered_map<std::string, int64_t> pids;
        for (const auto& m : activeModules) pids[m] = 42;
        return pids;
    }

    struct LaunchRecord {
        std::string name;
        std::string hostBinary;
        std::vector<std::string> args;
    };

    bool containerCanHandle = true;
    bool launchShouldSucceed = true;
    bool sendTokenResult = true;
    int terminateAllCount = 0;
    std::vector<LaunchRecord> launchCalls;
    std::vector<std::pair<std::string, std::string>> sendTokenCalls;
    std::vector<std::string> terminateCalls;
    std::unordered_set<std::string> activeModules;
};

struct FakeLoader : public ModuleLoader {
    std::string id() const override { return "fake-loader"; }
    bool canHandle(const ModuleDescriptor&) const override { return loaderCanHandle; }

    std::string resolveHostBinary(const ModuleDescriptor&) const override {
        return hostBinary;
    }

    std::vector<std::string> buildArguments(const ModuleDescriptor& desc) const override {
        return {"--name", desc.name, "--path", desc.path};
    }

    bool loaderCanHandle = true;
    std::string hostBinary = "/usr/bin/fake_host";
};

class CompositeRuntimeTest : public ::testing::Test {
protected:
    std::shared_ptr<FakeContainer> container;
    std::shared_ptr<FakeLoader> loader;
    std::shared_ptr<CompositeRuntime> runtime;

    void SetUp() override {
        container.reset(new FakeContainer);
        loader.reset(new FakeLoader);
        std::shared_ptr<ModuleContainer> c = container;
        std::shared_ptr<ModuleLoader> l = loader;
        runtime.reset(new CompositeRuntime(c, l));
    }
};

// ---------------------------------------------------------------------------
// id
// ---------------------------------------------------------------------------

TEST_F(CompositeRuntimeTest, Id_CombinesLoaderAndContainerIds) {
    EXPECT_EQ(runtime->id(), "fake-loader+fake-container");
}

// ---------------------------------------------------------------------------
// canHandle
// ---------------------------------------------------------------------------

TEST_F(CompositeRuntimeTest, Id_UsesOverrideWhenProvided) {
    auto rt = std::make_shared<CompositeRuntime>(
        std::static_pointer_cast<ModuleContainer>(container),
        std::static_pointer_cast<ModuleLoader>(loader),
        "my-custom-id");
    EXPECT_EQ(rt->id(), "my-custom-id");
}

TEST_F(CompositeRuntimeTest, Id_FallsBackWhenOverrideEmpty) {
    auto rt = std::make_shared<CompositeRuntime>(
        std::static_pointer_cast<ModuleContainer>(container),
        std::static_pointer_cast<ModuleLoader>(loader),
        "");
    EXPECT_EQ(rt->id(), "fake-loader+fake-container");
}

// ---------------------------------------------------------------------------
// canHandle
// ---------------------------------------------------------------------------

TEST_F(CompositeRuntimeTest, CanHandle_TrueWhenBothCanHandle) {
    ModuleDescriptor desc;
    EXPECT_TRUE(runtime->canHandle(desc));
}

TEST_F(CompositeRuntimeTest, CanHandle_FalseWhenContainerCannot) {
    container->containerCanHandle = false;
    ModuleDescriptor desc;
    EXPECT_FALSE(runtime->canHandle(desc));
}

TEST_F(CompositeRuntimeTest, CanHandle_FalseWhenLoaderCannot) {
    loader->loaderCanHandle = false;
    ModuleDescriptor desc;
    EXPECT_FALSE(runtime->canHandle(desc));
}

// ---------------------------------------------------------------------------
// load: resolves host + builds args via loader, launches via container
// ---------------------------------------------------------------------------

TEST_F(CompositeRuntimeTest, Load_DelegatesResolveAndLaunch) {
    ModuleDescriptor desc;
    desc.name = "mod_a";
    desc.path = "/lib/mod_a.so";
    LoadedModuleHandle handle;

    bool ok = runtime->load(desc, nullptr, handle);
    EXPECT_TRUE(ok);
    EXPECT_EQ(handle.name, "mod_a");
    EXPECT_EQ(handle.pid, 42);

    ASSERT_EQ(container->launchCalls.size(), 1u);
    EXPECT_EQ(container->launchCalls[0].name, "mod_a");
    EXPECT_EQ(container->launchCalls[0].hostBinary, "/usr/bin/fake_host");
    std::vector<std::string> expected = {"--name", "mod_a", "--path", "/lib/mod_a.so"};
    EXPECT_EQ(container->launchCalls[0].args, expected);
}

TEST_F(CompositeRuntimeTest, Load_FailsWhenHostBinaryEmpty) {
    loader->hostBinary = "";
    ModuleDescriptor desc;
    desc.name = "no_host";
    LoadedModuleHandle handle;

    EXPECT_FALSE(runtime->load(desc, nullptr, handle));
    EXPECT_TRUE(container->launchCalls.empty());
}

TEST_F(CompositeRuntimeTest, Load_FailsWhenContainerLaunchFails) {
    container->launchShouldSucceed = false;
    ModuleDescriptor desc;
    desc.name = "fail_launch";
    LoadedModuleHandle handle;

    EXPECT_FALSE(runtime->load(desc, nullptr, handle));
}

// ---------------------------------------------------------------------------
// sendToken, terminate, terminateAll delegate to container
// ---------------------------------------------------------------------------

TEST_F(CompositeRuntimeTest, SendToken_DelegatesToContainer) {
    EXPECT_TRUE(runtime->sendToken("mod_a", "tok123"));
    ASSERT_EQ(container->sendTokenCalls.size(), 1u);
    EXPECT_EQ(container->sendTokenCalls[0].first, "mod_a");
    EXPECT_EQ(container->sendTokenCalls[0].second, "tok123");
}

TEST_F(CompositeRuntimeTest, Terminate_DelegatesToContainer) {
    ModuleDescriptor desc;
    desc.name = "to_term";
    LoadedModuleHandle h;
    runtime->load(desc, nullptr, h);

    runtime->terminate("to_term");
    ASSERT_EQ(container->terminateCalls.size(), 1u);
    EXPECT_EQ(container->terminateCalls[0], "to_term");
}

TEST_F(CompositeRuntimeTest, TerminateAll_DelegatesToContainer) {
    runtime->terminateAll();
    EXPECT_EQ(container->terminateAllCount, 1);
}

// ---------------------------------------------------------------------------
// hasModule, pid, getAllPids delegate to container
// ---------------------------------------------------------------------------

TEST_F(CompositeRuntimeTest, HasModule_DelegatesToContainer) {
    EXPECT_FALSE(runtime->hasModule("x"));

    ModuleDescriptor desc;
    desc.name = "x";
    LoadedModuleHandle h;
    runtime->load(desc, nullptr, h);

    EXPECT_TRUE(runtime->hasModule("x"));
}

TEST_F(CompositeRuntimeTest, Pid_DelegatesToContainer) {
    EXPECT_FALSE(runtime->pid("x").has_value());

    ModuleDescriptor desc;
    desc.name = "x";
    LoadedModuleHandle h;
    runtime->load(desc, nullptr, h);

    auto p = runtime->pid("x");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, 42);
}

TEST_F(CompositeRuntimeTest, GetAllPids_DelegatesToContainer) {
    ModuleDescriptor d1;
    d1.name = "a";
    LoadedModuleHandle h1;
    runtime->load(d1, nullptr, h1);

    ModuleDescriptor d2;
    d2.name = "b";
    LoadedModuleHandle h2;
    runtime->load(d2, nullptr, h2);

    auto pids = runtime->getAllPids();
    EXPECT_EQ(pids.size(), 2u);
    EXPECT_EQ(pids.at("a"), 42);
    EXPECT_EQ(pids.at("b"), 42);
}

// ---------------------------------------------------------------------------
// Container accessor
// ---------------------------------------------------------------------------

TEST_F(CompositeRuntimeTest, ContainerAccessor_ReturnsSameInstance) {
    EXPECT_EQ(&runtime->container(), static_cast<ModuleContainer*>(container.get()));
}
