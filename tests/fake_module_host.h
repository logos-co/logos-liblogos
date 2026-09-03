#ifndef FAKE_MODULE_HOST_H
#define FAKE_MODULE_HOST_H

// A stand-in module host, and the fixture that drives the real load path
// against it. Shared by the load-verdict tests (what the caller is told when a
// child fails) and the load-concurrency tests (what two callers can do at once).

#include <gtest/gtest.h>

#include "logos_core.h"
#include "module_state_observer.h"
#include "qt_test_adapter.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct TmpDir {
    fs::path path;

    TmpDir() {
        std::string tmpl = (fs::temp_directory_path() / "logos_verdict_XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (!mkdtemp(buf.data())) throw std::runtime_error("mkdtemp failed");
        path = buf.data();
    }

    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// How long `slow-ok` stalls before reporting. Milliseconds, not seconds:
// thresholds derived from it divide, and integer seconds would silently floor
// (`seconds{1} * 3 / 2` is 1 s, not 1.5 s).
constexpr std::chrono::milliseconds kSlowHostDelay{1000};

// Stand-in for logos_host_qt. Its behaviour is the first line of the file the
// daemon names with --path, so one script covers every case and each module
// carries its own. `exec sleep` matters: a plain `sleep` would leave the shell
// as the process the container signals and the sleep behind it orphaned.
//
// Every invocation appends to "<path>.spawns" before it does anything else,
// which is how a test counts how many hosts were actually started for a module
// rather than inferring it from the answer the caller got.
constexpr const char* kFakeHostScript = R"sh(#!/bin/sh
path=""
while [ $# -gt 0 ]; do
  case "$1" in
    -p|--path) path="$2"; shift 2 ;;
    *) shift ;;
  esac
done
[ -n "$path" ] && echo spawn >> "$path.spawns"
case "$(head -n 1 "$path" 2>/dev/null)" in
  die)         exit 3 ;;
  report-fail) printf '%s\n' "@logos-load-status failed undefined symbol: logos_module_install" ; exit 1 ;;
  report-ok)   printf '%s\n' "@logos-load-status ok" ; exec sleep 300 ;;
  report-ok-then-die) printf '%s\n' "@logos-load-status ok" ; exit 0 ;;
  slow-ok)     sleep 1 ; printf '%s\n' "@logos-load-status ok" ; exec sleep 300 ;;
  *)           exec sleep 300 ;;
esac
)sh";

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
        unsetenv("LOGOS_HOST_PATH");
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

    // How many hosts the fake host script recorded for this module.
    int spawnCount(const std::string& name) const {
        std::ifstream f(tmp.path / (name + "_plugin.so.spawns"));
        int n = 0;
        for (std::string line; std::getline(f, line); ) ++n;
        return n;
    }

    // Installs the stand-in host for the duration of the test.
    void useFakeHost() {
        fs::path fakeHost = tmp.path / "fake_logos_host";
        std::ofstream f(fakeHost);
        f << kFakeHostScript;
        f.close();
        fs::permissions(fakeHost, fs::perms::owner_all | fs::perms::group_exec |
                                      fs::perms::others_exec);
        ASSERT_TRUE(fs::exists(fakeHost));
        setenv("LOGOS_HOST_PATH", fakeHost.c_str(), 1);
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
