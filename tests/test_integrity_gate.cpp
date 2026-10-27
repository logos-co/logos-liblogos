// The installed-module integrity gate's decision table, against real installed
// trees built with liblgx. The load path itself is covered by
// test_integrity_load.cpp.

#include <gtest/gtest.h>

#include "installed_module.h"
#include "integrity_gate.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using LogosCore::IntegrityGateDecision;
using LogosCore::evaluateIntegrityGate;

namespace {

class IntegrityGateTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string tmpl = (fs::temp_directory_path() / "logos_integrity_XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (!mkdtemp(buf.data())) throw std::runtime_error("mkdtemp failed");
        root = buf.data();
        modulesDir = root / "modules";
        dir = installed_module::install(root, modulesDir, "app");
        ASSERT_FALSE(dir.empty());
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    LogosCore::IntegrityGateResult check() const {
        return evaluateIntegrityGate(dir.string(), {modulesDir.string()});
    }

    fs::path root;
    fs::path modulesDir;
    fs::path dir;
};

TEST_F(IntegrityGateTest, AnUntouchedInstallIsAllowed) {
    EXPECT_EQ(check().decision, IntegrityGateDecision::Allow);
}

TEST_F(IntegrityGateTest, AModifiedFileIsRefused) {
    installed_module::writeText(dir / "app_plugin.so", "evil library content");

    const auto r = check();
    EXPECT_EQ(r.decision, IntegrityGateDecision::Refuse);
    EXPECT_NE(r.reason.find("modified or damaged since install"), std::string::npos) << r.reason;
}

TEST_F(IntegrityGateTest, AnAddedFileIsRefused) {
    installed_module::writeText(dir / "qml" / "Injected.qml", "import QtQuick\nItem {}\n");
    EXPECT_EQ(check().decision, IntegrityGateDecision::Refuse);
}

TEST_F(IntegrityGateTest, ADeletedFileIsRefused) {
    fs::remove(dir / "qml" / "Main.qml");
    EXPECT_EQ(check().decision, IntegrityGateDecision::Refuse);
}

// What lgpm writes beside the payload is not content: a catalog install records
// its download source, and must still pass.
TEST_F(IntegrityGateTest, TheInstallSourceSidecarIsAllowed) {
    installed_module::writeText(dir / ".lgpm-source", "https://example.org/repo\n");
    const auto r = check();
    EXPECT_EQ(r.decision, IntegrityGateDecision::Allow) << r.reason;
}

TEST_F(IntegrityGateTest, AMissingManifestIsRefused) {
    fs::remove(dir / "manifest.json");
    const auto r = check();
    EXPECT_EQ(r.decision, IntegrityGateDecision::Refuse);
    EXPECT_NE(r.reason.find("manifest.json"), std::string::npos) << r.reason;
}

TEST_F(IntegrityGateTest, AManifestWithoutHashesIsRefused) {
    std::ifstream in(dir / "manifest.json");
    nlohmann::json manifest = nlohmann::json::parse(in);
    in.close();
    manifest.erase("hashes");
    installed_module::writeText(dir / "manifest.json", manifest.dump());
    const auto r = check();
    EXPECT_EQ(r.decision, IntegrityGateDecision::Refuse);
    EXPECT_NE(r.reason.find("no content hashes"), std::string::npos) << r.reason;
}

TEST_F(IntegrityGateTest, AMissingVariantFileIsRefused) {
    fs::remove(dir / "variant");
    const auto r = check();
    EXPECT_EQ(r.decision, IntegrityGateDecision::Refuse);
    EXPECT_NE(r.reason.find("variant"), std::string::npos) << r.reason;
}

// Bundled modules and directories a developer passes in were never installed:
// even a changed one is not this gate's business.
TEST_F(IntegrityGateTest, AModuleOutsideEveryVerifiedDirIsNotChecked) {
    installed_module::writeText(dir / "app_plugin.so", "evil library content");
    const auto r = evaluateIntegrityGate(dir.string(), {(root / "elsewhere").string()});
    EXPECT_EQ(r.decision, IntegrityGateDecision::NotChecked);
}

TEST_F(IntegrityGateTest, AModuleWithNoInstallDirIsNotChecked) {
    EXPECT_EQ(evaluateIntegrityGate("", {modulesDir.string()}).decision,
              IntegrityGateDecision::NotChecked);
}

TEST_F(IntegrityGateTest, AVerifiedDirMatchesHoweverItIsSpelled) {
    installed_module::writeText(dir / "app_plugin.so", "evil library content");
    for (const std::string& spelling : {modulesDir.string() + "/",
                                        (modulesDir / ".").string(),
                                        (root / "modules" / ".." / "modules").string()}) {
        EXPECT_EQ(evaluateIntegrityGate(dir.string(), {spelling}).decision,
                  IntegrityGateDecision::Refuse)
            << spelling;
    }
}

} // namespace
