// Which loads may run at the same time, and which may not.
//
// One global loadMutex() guards the whole load path, so today every load is
// serialized against every other one regardless of which module it names.
// Bringing up a module is dominated by waiting for the child to report its
// verdict — tens of milliseconds warm, hundreds cold — and a frontend that
// loads several modules at startup pays that sum on one thread.
//
// The two halves are a pair, and the second is what stops the first from being
// "fixed" by removing the lock: DIFFERENT modules must overlap, the SAME module
// must not. Spawn counts come from the stand-in host itself rather than from
// what the caller was told, since the defect being guarded against is exactly a
// second child nobody asked for.
//
// The third case is a load inside a load, on ONE thread, which the same locks
// forbid for a different reason — see LoadReentrancyTest below.

#include "fake_module_host.h"

#include "module_loader.h"
#include "module_loader_registry.h"
#include "module_manager.h"
#include "subprocess_manager.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>

namespace {

class LoadConcurrencyTest : public FakeHostFixture {
protected:
    void SetUp() override {
        FakeHostFixture::SetUp();
        useFakeHost();
        // NO SINK, and it is not incidental. armReadinessWatch only arms when
        // one is installed, and arming it builds a LogosAPIClient and its
        // replica on the calling thread — here a worker that exits as soon as
        // the load returns, leaving Qt objects owned by a dead thread ("timers
        // can only be used with threads started with QThread", then a teardown
        // that never finishes). What these two tests pin is which loads may
        // overlap; the feed is covered by the load-verdict tests.
        logos::ModuleStateObserver::instance().setSink({});
    }
};

// Two modules that have nothing to do with each other must be able to come up
// at the same time. Each stand-in host stalls kSlowHostDelay before reporting,
// so serialized these cost two delays and overlapped they cost one.
TEST_F(LoadConcurrencyTest, DifferentModulesLoadConcurrently) {
    plantModule("alpha", "slow-ok");
    plantModule("beta", "slow-ok");

    const auto start = std::chrono::steady_clock::now();
    std::thread a([] { logos_core_load_module("alpha", false); });
    std::thread b([] { logos_core_load_module("beta", false); });
    a.join();
    b.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(logos_core_is_module_loaded("alpha"));
    EXPECT_TRUE(logos_core_is_module_loaded("beta"));
    // Two delays is the exact floor for a serialized pair — two sequential
    // sleeps of kSlowHostDelay cannot finish sooner — so anything under it is
    // proof they overlapped, and no arbitrary margin is involved.
    EXPECT_LT(elapsed, 2 * kSlowHostDelay)
        << "the two loads were serialized: "
        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        << " ms for two " << kSlowHostDelay.count() << " ms loads";
}

// The same module named by two callers at once is one load, not two. Both
// callers are told it is up — "already loaded" is a successful no-op, which
// callers rely on as "ensure loaded" — but only one host may be started.
TEST_F(LoadConcurrencyTest, SameModuleLoadsOnlyOnce) {
    plantModule("solo", "slow-ok");

    std::atomic<int> succeeded{0};
    std::thread a([&] { succeeded += logos_core_load_module("solo", false); });
    std::thread b([&] { succeeded += logos_core_load_module("solo", false); });
    a.join();
    b.join();

    EXPECT_EQ(succeeded.load(), 2);
    EXPECT_TRUE(logos_core_is_module_loaded("solo"));
    EXPECT_EQ(spawnCount("solo"), 1);
}

}  // namespace

// ── A load inside a load ────────────────────────────────────────────────────
//
// Not concurrency: one thread, re-entering. It happens because a call out to
// capability_module spins a nested Qt event loop, so a load a frontend posted
// with a queued connection can be delivered inside one already running.
// ModuleManager must refuse rather than proceed — proceeding takes fleetMutex's
// SHARED lock recursively, which is undefined behaviour and deadlocks against a
// queued writer, and capabilityRpcMutex, which is not recursive at all.
//
// Driven through the ModuleLoader seam because that is where the manager is
// holding those locks: load() is called with fleet shared and the module's own
// lock held, which is exactly the state a nested delivery arrives in.

namespace {

class ReenteringLoader : public LogosCore::ModuleLoader {
public:
    std::string id() const override { return "reentering"; }
    bool canHandle(const LogosCore::ModuleDescriptor&) const override { return true; }

    bool load(const LogosCore::ModuleDescriptor& desc,
              std::function<void(const std::string&)>,
              LogosCore::LoadedModuleHandle& out) override {
        if (!inner.empty() && !reentered) {
            reentered = true;
            innerResult = logos_core_load_module(inner.c_str(), false);
        }
        out.name = desc.name;
        out.pid = 4321;
        active.insert(desc.name);
        return true;
    }

    bool sendToken(const std::string&, const std::string&) override { return true; }
    void terminate(const std::string& name) override { active.erase(name); }
    void terminateAll() override { active.clear(); }
    bool hasModule(const std::string& name) const override { return active.count(name) > 0; }

    std::string inner;          // loaded from inside load(), once
    bool reentered = false;
    int innerResult = -1;

private:
    std::unordered_set<std::string> active;
};

class LoadReentrancyTest : public ::testing::Test {
protected:
    void SetUp() override {
        logos_core_terminate_all();
        logos_core_clear();
        loader = std::make_shared<ReenteringLoader>();
        ModuleManager::loaders().clearForTests();
        ModuleManager::loaders().registerLoader(loader);
    }

    void TearDown() override {
        logos_core_terminate_all();
        logos_core_clear();
        ModuleManager::loaders().clearForTests();
        ModuleManager::loaders().registerLoader(std::make_shared<SubprocessManager>());
    }

    void registerModule(const std::string& name) {
        const std::string path = "/fake/" + name + "_plugin.so";
        logos_core_register_module(name.c_str(), path.c_str());
    }

    std::shared_ptr<ReenteringLoader> loader;
};

TEST_F(LoadReentrancyTest, ALoadStartedInsideALoadIsRefused) {
    registerModule("outer");
    registerModule("inner");
    loader->inner = "inner";

    EXPECT_EQ(logos_core_load_module("outer", false), 1);

    ASSERT_TRUE(loader->reentered) << "the loader never re-entered, so this "
                                     "asserts nothing about re-entrancy";
    EXPECT_EQ(loader->innerResult, 0);
    EXPECT_FALSE(logos_core_is_module_loaded("inner"));
    EXPECT_TRUE(logos_core_is_module_loaded("outer"));
}

// The refusal must not break "ensure loaded": a caller asking for something
// that is already up gets the truthful yes, re-entrant or not.
TEST_F(LoadReentrancyTest, AReentrantLoadOfAnAlreadyLoadedModuleStillSucceeds) {
    registerModule("outer");
    registerModule("prior");
    ASSERT_EQ(logos_core_load_module("prior", false), 1);

    loader->inner = "prior";
    EXPECT_EQ(logos_core_load_module("outer", false), 1);

    ASSERT_TRUE(loader->reentered);
    EXPECT_EQ(loader->innerResult, 1);
}

}  // namespace
