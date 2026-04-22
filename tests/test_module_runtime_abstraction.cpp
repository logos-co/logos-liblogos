// =============================================================================
// Tests for the ModuleRuntime abstraction seam.
//
// Installs a FakeRuntime into ModuleManager's RuntimeRegistry and drives the
// full ModuleManager load/unload/terminateAll path. Proves that:
//   - load(), sendToken(), terminate(), terminateAll() are routed through the
//     runtime abstraction (not directly to a subprocess or Qt mechanism).
//   - Dependency-ordered loads call load() in the correct (topo) order.
//   - Error paths (load returns false) prevent sendToken from being called.
// No child processes are spawned; no Qt Remote Objects are used.
// =============================================================================
#include <gtest/gtest.h>
#include "logos_core.h"
#include "qt_test_adapter.h"
#include "module_manager.h"
#include "module_registry.h"
#include "runtime_registry.h"
#include "module_runtime.h"
#include "subprocess_manager.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>

using namespace LogosCore;

// ---------------------------------------------------------------------------
// FakeRuntime: records all calls; configurable per-module load result.
// Placed in an anonymous namespace to avoid ODR conflicts with the FakeRuntime
// stub in test_runtime_registry.cpp (same binary, different definition).
// ---------------------------------------------------------------------------
namespace {

struct FakeRuntime : public ModuleRuntime {
    std::string id() const override { return "fake"; }

    bool canHandle(const ModuleDescriptor&) const override { return true; }

    bool load(const ModuleDescriptor& desc,
              std::function<void(const std::string&)>,
              LoadedModuleHandle& out) override {
        loadCalls.push_back(desc.name);
        if (failOn.count(desc.name)) return false;
        out.name = desc.name;
        out.pid  = 1234;
        out.endpoint = "fake://" + desc.name;
        activeModules.insert(desc.name);
        return true;
    }

    bool sendToken(const std::string& name, const std::string& token) override {
        sendTokenCalls.push_back({name, token});
        return true;
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

    // Call records
    std::vector<std::string>                         loadCalls;
    std::vector<std::pair<std::string,std::string>>  sendTokenCalls;
    std::vector<std::string>                         terminateCalls;
    int                                              terminateAllCount = 0;

    // Modules to fail on load
    std::unordered_set<std::string>                  failOn;
    // Modules currently "running"
    std::unordered_set<std::string>                  activeModules;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test fixture: installs FakeRuntime, cleans up registry after each test.
// ---------------------------------------------------------------------------

class ModuleRuntimeAbstractionTest : public ::testing::Test {
protected:
    std::shared_ptr<FakeRuntime> fake;

    void SetUp() override {
        logos_core_terminate_all();
        logos_core_clear();
        SubprocessManager::clearAll();

        fake = std::make_shared<FakeRuntime>();
        ModuleManager::runtimes().clearForTests();
        ModuleManager::runtimes().registerRuntime(fake);
    }

    void TearDown() override {
        logos_core_terminate_all();
        logos_core_clear();
        SubprocessManager::clearAll();
        // Restore default runtime so other test suites aren't affected.
        ModuleManager::runtimes().clearForTests();
        ModuleManager::runtimes().registerRuntime(
            std::make_shared<SubprocessManager>());
    }

    void registerModule(const std::string& name,
                        const std::vector<std::string>& deps = {}) {
        std::string path = "/fake/" + name + "_plugin.so";
        logos_core_register_module(name.c_str(), path.c_str());
        std::vector<const char*> depPtrs;
        for (const auto& d : deps) depPtrs.push_back(d.c_str());
        logos_core_register_module_dependencies(
            name.c_str(),
            depPtrs.empty() ? nullptr : depPtrs.data(),
            static_cast<int>(depPtrs.size()));
    }
};

// =============================================================================
// Basic load/unload routing
// =============================================================================

TEST_F(ModuleRuntimeAbstractionTest, LoadModule_CallsFakeRuntimeLoad) {
    registerModule("foo");

    int result = logos_core_load_module("foo");
    ASSERT_EQ(result, 1);

    ASSERT_EQ(fake->loadCalls.size(), 1u);
    EXPECT_EQ(fake->loadCalls[0], "foo");
}

TEST_F(ModuleRuntimeAbstractionTest, LoadModule_CallsSendTokenAfterLoad) {
    registerModule("foo");

    logos_core_load_module("foo");

    ASSERT_EQ(fake->sendTokenCalls.size(), 1u);
    EXPECT_EQ(fake->sendTokenCalls[0].first, "foo");
    EXPECT_FALSE(fake->sendTokenCalls[0].second.empty());
}

TEST_F(ModuleRuntimeAbstractionTest, LoadModule_MarksModuleAsLoaded) {
    registerModule("foo");

    logos_core_load_module("foo");

    EXPECT_EQ(logos_core_is_module_loaded("foo"), 1);
}

TEST_F(ModuleRuntimeAbstractionTest, LoadModule_StoresRuntimeInRegistry) {
    registerModule("foo");
    logos_core_load_module("foo");

    auto rt = ModuleManager::registry().runtimeFor("foo");
    EXPECT_EQ(rt.get(), fake.get());
}

TEST_F(ModuleRuntimeAbstractionTest, UnloadModule_CallsFakeRuntimeTerminate) {
    registerModule("foo");
    logos_core_load_module("foo");

    int result = logos_core_unload_module("foo");
    ASSERT_EQ(result, 1);

    ASSERT_EQ(fake->terminateCalls.size(), 1u);
    EXPECT_EQ(fake->terminateCalls[0], "foo");
}

TEST_F(ModuleRuntimeAbstractionTest, UnloadModule_MarksModuleAsUnloaded) {
    registerModule("foo");
    logos_core_load_module("foo");
    logos_core_unload_module("foo");

    EXPECT_EQ(logos_core_is_module_loaded("foo"), 0);
}

// =============================================================================
// Dependency-ordered loads
// =============================================================================

TEST_F(ModuleRuntimeAbstractionTest, LoadWithDeps_LoadsInTopologicalOrder) {
    // Chain: c depends on b, b depends on a.
    // Expected load order: a, b, c.
    registerModule("a");
    registerModule("b", {"a"});
    registerModule("c", {"b"});

    int result = logos_core_load_module_with_dependencies("c");
    ASSERT_EQ(result, 1);

    ASSERT_EQ(fake->loadCalls.size(), 3u);
    EXPECT_EQ(fake->loadCalls[0], "a");
    EXPECT_EQ(fake->loadCalls[1], "b");
    EXPECT_EQ(fake->loadCalls[2], "c");
}

TEST_F(ModuleRuntimeAbstractionTest, LoadWithDeps_SkipsAlreadyLoadedModules) {
    registerModule("a");
    registerModule("b", {"a"});

    logos_core_load_module("a");
    fake->loadCalls.clear();

    logos_core_load_module_with_dependencies("b");

    ASSERT_EQ(fake->loadCalls.size(), 1u);
    EXPECT_EQ(fake->loadCalls[0], "b");
}

// =============================================================================
// terminateAll routing
// =============================================================================

TEST_F(ModuleRuntimeAbstractionTest, TerminateAll_CallsFakeTerminateAll) {
    registerModule("foo");
    logos_core_load_module("foo");

    logos_core_terminate_all();

    EXPECT_EQ(fake->terminateAllCount, 1);
    EXPECT_EQ(logos_core_is_module_loaded("foo"), 0);
}

// =============================================================================
// Error paths
// =============================================================================

TEST_F(ModuleRuntimeAbstractionTest, LoadModule_ReturnsFalseWhenRuntimeLoadFails) {
    registerModule("bad");
    fake->failOn.insert("bad");

    int result = logos_core_load_module("bad");
    EXPECT_EQ(result, 0);
}

TEST_F(ModuleRuntimeAbstractionTest, LoadModule_DoesNotCallSendTokenOnLoadFailure) {
    registerModule("bad");
    fake->failOn.insert("bad");

    logos_core_load_module("bad");

    EXPECT_TRUE(fake->sendTokenCalls.empty());
}

TEST_F(ModuleRuntimeAbstractionTest, LoadModule_DoesNotMarkAsLoadedOnFailure) {
    registerModule("bad");
    fake->failOn.insert("bad");

    logos_core_load_module("bad");

    EXPECT_EQ(logos_core_is_module_loaded("bad"), 0);
}

TEST_F(ModuleRuntimeAbstractionTest, LoadModule_ReturnsFalseForUnknownModule) {
    int result = logos_core_load_module("not_registered");
    EXPECT_EQ(result, 0);
    EXPECT_TRUE(fake->loadCalls.empty());
}
