#include <gtest/gtest.h>
#include "containers/sandbox/sandbox_container.h"
#include <string>

class SandboxContainerTest : public ::testing::Test {
protected:
    SandboxContainer container;
};

// ---------------------------------------------------------------------------
// id / canHandle
// ---------------------------------------------------------------------------

TEST_F(SandboxContainerTest, Id_ReturnsSandbox) {
    EXPECT_EQ(container.id(), "sandbox");
}

TEST_F(SandboxContainerTest, CanHandle_ReturnsFalseForPlainDescriptor) {
    LogosCore::ModuleDescriptor desc;
    desc.format = "qt-plugin";
    EXPECT_FALSE(container.canHandle(desc));
}

TEST_F(SandboxContainerTest, CanHandle_ReturnsFalseAlways) {
    LogosCore::ModuleDescriptor desc;
    desc.runtimeConfig["id"] = "sandbox";
    EXPECT_FALSE(container.canHandle(desc));
}

// ---------------------------------------------------------------------------
// generateProfile: default policy
// ---------------------------------------------------------------------------

TEST_F(SandboxContainerTest, GenerateProfile_ContainsDenyDefault) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "test_mod";
    std::string profile = SandboxContainer::generateProfile(desc, "/usr/bin/host");
    EXPECT_NE(profile.find("(version 1)"), std::string::npos);
    EXPECT_NE(profile.find("(deny default)"), std::string::npos);
}

TEST_F(SandboxContainerTest, GenerateProfile_AllowsSystemLibraries) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "test_mod";
    std::string profile = SandboxContainer::generateProfile(desc, "/usr/bin/host");
    EXPECT_NE(profile.find("(subpath \"/usr/lib\")"), std::string::npos);
    EXPECT_NE(profile.find("(subpath \"/System/Library\")"), std::string::npos);
}

TEST_F(SandboxContainerTest, GenerateProfile_AllowsHostBinary) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "test_mod";
    std::string profile = SandboxContainer::generateProfile(desc, "/opt/bin/logos_host_qt");
    EXPECT_NE(profile.find("(allow file-read* (literal \"/opt/bin/logos_host_qt\"))"),
              std::string::npos);
    EXPECT_NE(profile.find("(allow process-exec (literal \"/opt/bin/logos_host_qt\"))"),
              std::string::npos);
}

TEST_F(SandboxContainerTest, GenerateProfile_AllowsModulePath) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "test_mod";
    desc.path = "/modules/test_mod.so";
    std::string profile = SandboxContainer::generateProfile(desc, "/usr/bin/host");
    EXPECT_NE(profile.find("(allow file-read* (subpath \"/modules/test_mod.so\"))"),
              std::string::npos);
}

TEST_F(SandboxContainerTest, GenerateProfile_AllowsTokenSocket) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "test_mod";
    std::string profile = SandboxContainer::generateProfile(desc, "/usr/bin/host");
    EXPECT_NE(profile.find("(allow network-unix)"), std::string::npos);
    EXPECT_NE(profile.find("logos_token_test_mod"), std::string::npos);
}

TEST_F(SandboxContainerTest, GenerateProfile_AllowsMachAndSysctl) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "test_mod";
    std::string profile = SandboxContainer::generateProfile(desc, "/usr/bin/host");
    EXPECT_NE(profile.find("(allow sysctl-read)"), std::string::npos);
    EXPECT_NE(profile.find("(allow mach-lookup)"), std::string::npos);
}

// ---------------------------------------------------------------------------
// generateProfile: persistence path
// ---------------------------------------------------------------------------

TEST_F(SandboxContainerTest, GenerateProfile_AllowsPersistencePath) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "test_mod";
    desc.instancePersistencePath = "/var/data/test_mod/instance1";
    std::string profile = SandboxContainer::generateProfile(desc, "/usr/bin/host");
    EXPECT_NE(profile.find("(allow file-read* file-write* (subpath \"/var/data/test_mod/instance1\"))"),
              std::string::npos);
}

TEST_F(SandboxContainerTest, GenerateProfile_OmitsPersistenceWhenEmpty) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "test_mod";
    std::string profile = SandboxContainer::generateProfile(desc, "/usr/bin/host");
    EXPECT_EQ(profile.find("persistence"), std::string::npos);
}

// ---------------------------------------------------------------------------
// generateProfile: network configuration
// ---------------------------------------------------------------------------

TEST_F(SandboxContainerTest, GenerateProfile_NoNetworkByDefault) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "test_mod";
    std::string profile = SandboxContainer::generateProfile(desc, "/usr/bin/host");
    EXPECT_EQ(profile.find("network-outbound"), std::string::npos);
    EXPECT_EQ(profile.find("network-inbound"), std::string::npos);
}

TEST_F(SandboxContainerTest, GenerateProfile_NetworkOutboundAll) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "test_mod";
    desc.runtimeConfig["network"]["outbound"] = true;
    std::string profile = SandboxContainer::generateProfile(desc, "/usr/bin/host");
    EXPECT_NE(profile.find("(allow network-outbound)"), std::string::npos);
}

TEST_F(SandboxContainerTest, GenerateProfile_NetworkOutboundSpecific) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "test_mod";
    desc.runtimeConfig["network"]["outbound"] = nlohmann::json::array({"localhost:5432", "*:443"});
    std::string profile = SandboxContainer::generateProfile(desc, "/usr/bin/host");
    EXPECT_NE(profile.find("(remote tcp \"localhost:5432\")"), std::string::npos);
    EXPECT_NE(profile.find("(remote tcp \"*:443\")"), std::string::npos);
}

TEST_F(SandboxContainerTest, GenerateProfile_NetworkInboundAll) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "test_mod";
    desc.runtimeConfig["network"]["inbound"] = true;
    std::string profile = SandboxContainer::generateProfile(desc, "/usr/bin/host");
    EXPECT_NE(profile.find("(allow network-inbound)"), std::string::npos);
}

TEST_F(SandboxContainerTest, GenerateProfile_NetworkInboundSpecific) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "test_mod";
    desc.runtimeConfig["network"]["inbound"] = nlohmann::json::array({8080, 3000});
    std::string profile = SandboxContainer::generateProfile(desc, "/usr/bin/host");
    EXPECT_NE(profile.find("(local tcp \"*:8080\")"), std::string::npos);
    EXPECT_NE(profile.find("(local tcp \"*:3000\")"), std::string::npos);
}

// ---------------------------------------------------------------------------
// generateProfile: filesystem configuration
// ---------------------------------------------------------------------------

TEST_F(SandboxContainerTest, GenerateProfile_FilesystemReadPaths) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "test_mod";
    desc.runtimeConfig["filesystem"]["read"] = nlohmann::json::array({"/data/readonly"});
    std::string profile = SandboxContainer::generateProfile(desc, "/usr/bin/host");
    EXPECT_NE(profile.find("(allow file-read* (subpath \"/data/readonly\"))"),
              std::string::npos);
}

TEST_F(SandboxContainerTest, GenerateProfile_FilesystemReadWritePaths) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "test_mod";
    desc.runtimeConfig["filesystem"]["read-write"] = nlohmann::json::array({"/data/rw"});
    std::string profile = SandboxContainer::generateProfile(desc, "/usr/bin/host");
    EXPECT_NE(profile.find("(allow file-read* file-write* (subpath \"/data/rw\"))"),
              std::string::npos);
}

// ---------------------------------------------------------------------------
// generateProfile: modules directories
// ---------------------------------------------------------------------------

TEST_F(SandboxContainerTest, GenerateProfile_AllowsModulesDirs) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "test_mod";
    desc.modulesDirs = {"/opt/modules", "/usr/local/modules"};
    std::string profile = SandboxContainer::generateProfile(desc, "/usr/bin/host");
    EXPECT_NE(profile.find("(allow file-read* (subpath \"/opt/modules\"))"),
              std::string::npos);
    EXPECT_NE(profile.find("(allow file-read* (subpath \"/usr/local/modules\"))"),
              std::string::npos);
}

// ---------------------------------------------------------------------------
// launch: non-macOS graceful failure
// ---------------------------------------------------------------------------

TEST_F(SandboxContainerTest, Launch_FailsOnNonMacOS) {
#if defined(__APPLE__)
    GTEST_SKIP() << "This test is for non-macOS platforms";
#endif
    LogosCore::ModuleDescriptor desc;
    desc.name = "sandbox_mod";
    desc.runtimeConfig["id"] = "sandbox";
    LogosCore::LoadedModuleHandle handle;
    EXPECT_FALSE(container.launch(desc, "/bin/sleep", {"5"}, nullptr, handle));
}
