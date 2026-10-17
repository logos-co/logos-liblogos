#ifndef FAKE_MODULE_HOST_H
#define FAKE_MODULE_HOST_H

// A stand-in module host, and the fixture that drives the real load path
// against it. Shared by the load-verdict tests (what the caller is told when a
// child fails) and the load-concurrency tests (what two callers can do at once).

#include <gtest/gtest.h>

#include "logos_core.h"
#include "module_state_observer.h"
#include "qt_test_adapter.h"
#include "test_platform.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using logos_test::TmpDir;

// The stand-in for logos_host_qt is tests/fake_module_host_main.cpp. Its
// behaviour is the first line of the file the daemon names with --path, so one
// host covers every case and each module carries its own:
//
//   die                 exit 3 before reporting anything
//   report-fail         report a plugin that failed to load, exit 1
//   report-ok           report ok and stay up
//   report-ok-then-die  report ok and exit
//   slow-ok             stall a whole second, then report ok and stay up
//   pdeathsig-ok        arm PR_SET_PDEATHSIG as logos_host_qt does (Linux),
//                       then report ok and stay up
//   anything else       stay up without a word, as a host too old to report
//
// `slow-ok`'s second is not a timeout to be waited out: it is the window a test
// needs a second host to turn up inside, wide enough that a loaded machine
// cannot close it.
//
// Every host writes two marks into ONE log shared by every module in the
// directory: `enter` the moment it starts, `report` as it is about to answer.
// One shared log rather than a file per module because the order of the marks
// across modules is itself the evidence -- see hostWindow() below -- and each
// mark is one write to an append-only handle, so the file records the real
// sequence even with several hosts running at once.

std::set<std::string> loadedModuleNames() {
    std::set<std::string> names;
    char** arr = logos_core_get_loaded_modules();
    if (!arr) return names;
    for (int i = 0; arr[i]; ++i) {
        names.insert(arr[i]);
        delete[] arr[i];
    }
    delete[] arr;
    return names;
}

// A module is registered as known with a placeholder binary, the load runs for
// real, and the lifecycle feed is captured.
class FakeHostFixture : public ::testing::Test {
protected:
    void SetUp() override {
        logos_core_terminate_all();
        logos_core_clear();

        auto& o = logos::ModuleStateObserver::instance();
        o.setSink({});
        o.clearPending();
        seen.clear();
        // Locked: a load reports on the thread that performed it, so with
        // concurrent loads two flushes reach this sink at once.
        o.setSink([this](const std::vector<logos::ModuleTransition>& batch) {
            std::lock_guard<std::mutex> g(seenMutex);
            for (const auto& t : batch) seen.push_back(t);
        });
    }

    void TearDown() override {
        auto& o = logos::ModuleStateObserver::instance();
        o.setSink({});
        o.clearPending();
        logos_test::unsetEnv("LOGOS_HOST_PATH");
        logos_core_terminate_all();
        logos_core_clear();
    }

    void plantModule(const std::string& name, const std::string& contents) {
        fs::path binary = tmp.path / (name + "_plugin.so");
        std::ofstream f(binary);
        f << contents << "\n";
        f.close();
        logos_core_register_module(name.c_str(), binary.string().c_str());
        ASSERT_TRUE(logos_core_is_module_known(name.c_str()));
    }

    // Where a module's host sat in the shared log: the mark that started it,
    // and the mark where it reported its verdict. -1 for a host that never got
    // that far.
    //
    // POSITIONS, not clock readings, and that is the whole point. A busy
    // machine stretches every interval a test could time -- it cannot reorder
    // two marks -- so a question asked of these indices gets the same answer on
    // an idle machine and on one under load.
    struct HostWindow {
        int entered = -1;
        int reported = -1;
    };

    HostWindow hostWindow(const std::string& name) const {
        HostWindow w;
        const std::vector<std::string> events = hostEvents();
        for (int i = 0; i < static_cast<int>(events.size()); ++i) {
            if (w.entered < 0 && events[i] == "enter " + name) w.entered = i;
            if (w.reported < 0 && events[i] == "report " + name) w.reported = i;
        }
        return w;
    }

    // How many hosts the stand-in recorded for this module.
    int spawnCount(const std::string& name) const {
        int n = 0;
        for (const std::string& e : hostEvents())
            if (e == "enter " + name) ++n;
        return n;
    }

    // Every mark from every host in this test, in order. Worth attaching to a
    // failure: it says what actually happened, which "expected 3 < 1" does not.
    std::vector<std::string> hostEvents() const {
        std::ifstream f(tmp.path / "host_events");
        std::vector<std::string> lines;
        for (std::string line; std::getline(f, line); ) lines.push_back(line);
        return lines;
    }

    std::string hostEventLog() const {
        std::string out;
        for (const std::string& e : hostEvents()) out += "\n  " + e;
        return out;
    }

    // Installs the stand-in host for the duration of the test.
    void useFakeHost() {
        const fs::path fakeHost = logos_test::fakeHostPath();
        ASSERT_TRUE(fs::exists(fakeHost)) << fakeHost;
        logos_test::setEnv("LOGOS_HOST_PATH", fakeHost.string());
    }

    bool sawTransitionTo(const std::string& name, const std::string& state) const {
        for (const auto& t : seen)
            if (t.module == name && t.newState == state) return true;
        return false;
    }

    std::string reasonFor(const std::string& name, const std::string& state) const {
        for (const auto& t : seen)
            if (t.module == name && t.newState == state && t.reason.has_value())
                return *t.reason;
        return {};
    }

    TmpDir tmp;
    std::mutex seenMutex;
    std::vector<logos::ModuleTransition> seen;
};

}  // namespace

#endif  // FAKE_MODULE_HOST_H
