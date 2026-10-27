// The real load path refusing what the integrity gate refuses, the reason a
// caller can read back, and a refused module's dependents not starting without
// it. Driven against the stand-in host, so "never started" is observable.

#include "fake_module_host.h"
#include "installed_module.h"

#include <cstdlib>
#include <memory>
#include <string>

namespace {

class IntegrityLoadTest : public FakeHostFixture {
protected:
    void SetUp() override {
        FakeHostFixture::SetUp();
        useFakeHost();
        modulesDir = tmp.path / "modules";
    }

    // `name` is known, starts cleanly on the stand-in host, and is installed
    // under the verified modules dir.
    fs::path plantInstalled(const std::string& name) {
        plantModule(name, "report-ok");
        const fs::path dir = installed_module::install(tmp.path, modulesDir, name);
        EXPECT_FALSE(dir.empty());
        ModuleManager::registry().setModuleInstallDir(name, dir.string());
        logos_core_add_verified_modules_dir(modulesDir.string().c_str());
        return dir;
    }

    static std::string lastError(const char* name) {
        std::unique_ptr<char[]> s(logos_core_get_last_load_error(name));
        return s ? std::string(s.get()) : std::string();
    }

    fs::path modulesDir;
};

TEST_F(IntegrityLoadTest, AnUntouchedInstalledModuleLoads) {
    plantInstalled("app");

    EXPECT_EQ(logos_core_load_module("app", LOGOS_LOAD_MODULE_ONLY), 1);
    EXPECT_EQ(lastError("app"), "");
}

TEST_F(IntegrityLoadTest, AModifiedInstalledModuleIsNeverStarted) {
    const fs::path dir = plantInstalled("app");
    installed_module::writeText(dir / "app_plugin.so", "evil library content");

    EXPECT_EQ(logos_core_load_module("app", LOGOS_LOAD_MODULE_ONLY), 0);
    EXPECT_EQ(spawnCount("app"), 0) << hostEventLog();
    EXPECT_NE(lastError("app").find("modified or damaged since install"), std::string::npos)
        << lastError("app");
    EXPECT_NE(reasonFor("app", logos::module_state::kError).find("modified"), std::string::npos);
}

// Only what the package manager installed is checked.
TEST_F(IntegrityLoadTest, AModuleOutsideTheVerifiedDirIsNotChecked) {
    plantModule("dev", "report-ok");
    const fs::path dir = installed_module::install(tmp.path, tmp.path / "dev_build", "dev");
    ASSERT_FALSE(dir.empty());
    ModuleManager::registry().setModuleInstallDir("dev", dir.string());
    logos_core_add_verified_modules_dir(modulesDir.string().c_str());
    installed_module::writeText(dir / "dev_plugin.so", "edited by the developer");

    EXPECT_EQ(logos_core_load_module("dev", LOGOS_LOAD_MODULE_ONLY), 1);
}

// Repairing the module and loading again works, and the stale reason goes.
TEST_F(IntegrityLoadTest, ASuccessfulLoadClearsThePreviousError) {
    const fs::path dir = plantInstalled("app");
    installed_module::writeText(dir / "app_plugin.so", "evil library content");
    ASSERT_EQ(logos_core_load_module("app", LOGOS_LOAD_MODULE_ONLY), 0);
    ASSERT_FALSE(lastError("app").empty());

    installed_module::writeText(dir / "app_plugin.so", "fake library content");
    EXPECT_EQ(logos_core_load_module("app", LOGOS_LOAD_MODULE_ONLY), 1);
    EXPECT_EQ(lastError("app"), "");
}

// A module whose required dependency failed must not start without it. Before,
// the loop logged the failure and loaded the dependent anyway.
TEST_F(IntegrityLoadTest, ADependentOfAFailedModuleIsNeverStarted) {
    plantModule("base", "die");
    plantModule("app", "report-ok");
    const char* deps[] = {"base"};
    logos_core_register_module_dependencies("app", deps, 1);

    EXPECT_EQ(logos_core_load_module("app", LOGOS_LOAD_REQUIRED_DEPS), 0);
    EXPECT_EQ(spawnCount("app"), 0) << hostEventLog();
    EXPECT_NE(lastError("app").find("requires base, which failed to load"), std::string::npos)
        << lastError("app");
}

// The case the two halves exist for: one damaged module, and the app on top of
// it says why it didn't start.
TEST_F(IntegrityLoadTest, ADependentOfAModifiedModuleSaysWhy) {
    const fs::path dir = plantInstalled("base");
    plantModule("app", "report-ok");
    const char* deps[] = {"base"};
    logos_core_register_module_dependencies("app", deps, 1);
    installed_module::writeText(dir / "base_plugin.so", "evil library content");

    EXPECT_EQ(logos_core_load_module("app", LOGOS_LOAD_REQUIRED_DEPS), 0);
    EXPECT_EQ(spawnCount("base"), 0) << hostEventLog();
    EXPECT_EQ(spawnCount("app"), 0) << hostEventLog();
    const std::string why = lastError("app");
    EXPECT_NE(why.find("requires base"), std::string::npos) << why;
    EXPECT_NE(why.find("modified or damaged since install"), std::string::npos) << why;
}

// The on-demand check a frontend runs on the UI plugins it loads itself.
TEST_F(IntegrityLoadTest, VerifyInstalledModuleAnswersNullWhenIntactAndWhyWhenNot) {
    const fs::path dir = installed_module::install(tmp.path, modulesDir, "ui_app");
    ASSERT_FALSE(dir.empty());
    auto verify = [](const std::string& d) {
        std::unique_ptr<char[]> s(logos_core_verify_installed_module(d.c_str()));
        return s ? std::string(s.get()) : std::string();
    };

    EXPECT_EQ(verify(dir.string()), "");

    installed_module::writeText(dir / "qml" / "Main.qml", "import QtQuick\nItem { id: evil }\n");
    EXPECT_NE(verify(dir.string()).find("modified or damaged since install"), std::string::npos);
    // However the directory is spelled, it is checked: never "intact" by default.
    EXPECT_NE(verify(dir.string() + "/").find("modified"), std::string::npos);
    EXPECT_NE(verify("").find("no install directory"), std::string::npos);
}

// The feed gets a verdict for the module that was asked for, not only for the
// dependency that failed under it.
TEST_F(IntegrityLoadTest, ADependentOfAFailedModuleIsAnErrorOnTheFeed) {
    plantModule("base", "die");
    plantModule("app", "report-ok");
    const char* deps[] = {"base"};
    logos_core_register_module_dependencies("app", deps, 1);

    ASSERT_EQ(logos_core_load_module("app", LOGOS_LOAD_REQUIRED_DEPS), 0);
    EXPECT_TRUE(sawTransitionTo("app", logos::module_state::kError));
    EXPECT_NE(reasonFor("app", logos::module_state::kError).find("requires base"),
              std::string::npos);
}

// Failures before any module is attempted carry a reason too.
TEST_F(IntegrityLoadTest, AMissingRequiredDependencySaysWhich) {
    plantModule("app", "report-ok");
    const char* deps[] = {"ghost_dep"};
    logos_core_register_module_dependencies("app", deps, 1);

    EXPECT_EQ(logos_core_load_module("app", LOGOS_LOAD_REQUIRED_DEPS), 0);
    EXPECT_EQ(lastError("app"), "requires ghost_dep, which is not installed");
    EXPECT_TRUE(sawTransitionTo("app", logos::module_state::kError));
}

TEST_F(IntegrityLoadTest, ADependencyCycleSaysSo) {
    plantModule("a", "report-ok");
    plantModule("b", "report-ok");
    const char* aDeps[] = {"b"};
    const char* bDeps[] = {"a"};
    logos_core_register_module_dependencies("a", aDeps, 1);
    logos_core_register_module_dependencies("b", bDeps, 1);

    EXPECT_EQ(logos_core_load_module("a", LOGOS_LOAD_REQUIRED_DEPS), 0);
    EXPECT_EQ(lastError("a"), "its dependencies form a cycle");
}

TEST_F(IntegrityLoadTest, AnUninstalledModuleSaysSoThroughTheDependencyPath) {
    EXPECT_EQ(logos_core_load_module("nowhere", LOGOS_LOAD_REQUIRED_DEPS), 0);
    EXPECT_EQ(lastError("nowhere"), "module is not installed");
}

// A new failure replaces the old reason rather than leaving a stale one.
TEST_F(IntegrityLoadTest, AResolutionFailureReplacesAnEarlierReason) {
    ASSERT_EQ(logos_core_load_module("app", LOGOS_LOAD_REQUIRED_DEPS), 0);
    ASSERT_EQ(lastError("app"), "module is not installed");

    plantModule("app", "report-ok");
    const char* deps[] = {"ghost_dep"};
    logos_core_register_module_dependencies("app", deps, 1);
    EXPECT_EQ(logos_core_load_module("app", LOGOS_LOAD_REQUIRED_DEPS), 0);
    EXPECT_EQ(lastError("app"), "requires ghost_dep, which is not installed");
}

TEST_F(IntegrityLoadTest, AnUnknownModuleSaysSo) {
    EXPECT_EQ(logos_core_load_module("nowhere", LOGOS_LOAD_MODULE_ONLY), 0);
    EXPECT_EQ(lastError("nowhere"), "module is not installed");
}

} // namespace
