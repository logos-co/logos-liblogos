// =============================================================================
// Unit tests for logos::isValidModuleName — the allowlist that guards an
// untrusted module name before it is used as a path segment or identifier.
//
// A module's name comes verbatim from untrusted plugin metadata. It is used as
// the registry map key, the LogosAPI RPC target, and the instance-persistence
// directory segment (basePath + "/" + name + "/" + instanceId). A name
// containing '/' or ".." would escape the intended directory or collide with
// another module, so ModuleRegistry::processModuleInternal rejects any name
// that is not a safe identifier at the trust boundary.
//
// (The token handoff no longer derives a filesystem path from the name — it is
// delivered over the child's stdin pipe by the subprocess container — so the
// old socket-path Sender/Receiver tests are gone. The allowlist still matters
// for the registry key / RPC target / persistence path, which is what these
// tests pin.)
// =============================================================================
#include <gtest/gtest.h>
#include "module_name_validation.h"

#include <string>

TEST(ModuleNameValidation, AcceptsRealModuleNames) {
    // Names that ship today in the workspace's metadata.json files.
    EXPECT_TRUE(::logos::isValidModuleName("chat"));
    EXPECT_TRUE(::logos::isValidModuleName("chat_module"));
    EXPECT_TRUE(::logos::isValidModuleName("waku_module"));
    EXPECT_TRUE(::logos::isValidModuleName("accounts_ui"));
    EXPECT_TRUE(::logos::isValidModuleName("package-downloader"));
    EXPECT_TRUE(::logos::isValidModuleName("Counter123"));
}

TEST(ModuleNameValidation, RejectsPathSeparatorsAndTraversal) {
    EXPECT_FALSE(::logos::isValidModuleName("../evil"));
    EXPECT_FALSE(::logos::isValidModuleName("../../etc/passwd"));
    EXPECT_FALSE(::logos::isValidModuleName("a/b"));
    EXPECT_FALSE(::logos::isValidModuleName("foo\\bar"));
    EXPECT_FALSE(::logos::isValidModuleName(".."));
    EXPECT_FALSE(::logos::isValidModuleName("."));
}

TEST(ModuleNameValidation, RejectsEmptyControlAndOverlong) {
    EXPECT_FALSE(::logos::isValidModuleName(""));
    EXPECT_FALSE(::logos::isValidModuleName(std::string("evil\0hidden", 10)));  // embedded NUL
    EXPECT_FALSE(::logos::isValidModuleName("has space"));
    EXPECT_FALSE(::logos::isValidModuleName("dot.name"));     // '.' is not in the allowlist
    EXPECT_FALSE(::logos::isValidModuleName(std::string(65, 'a')));  // > 64 bytes
    EXPECT_TRUE(::logos::isValidModuleName(std::string(64, 'a')));   // boundary: 64 ok
}
