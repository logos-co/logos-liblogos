// =============================================================================
// Tests for the access-policy parser (src/logos_core/access_policy.{h,cpp}).
//
// parseAccessPolicy turns the JSON document set via
// logos_core_set_access_policy into a structured AccessPolicy. Core uses it to
// register concrete per-target restrictions with capability_module; enforcement
// itself lives in capability_module (tested there). These tests pin the parse
// contract:
//   - the exact basecamp/daemon production document parses correctly
//   - mode "off", or none, is reflected via enforce()==false (core registers nothing)
//   - tolerant where it cannot open a target: unknown keys ignored, missing
//     restrictions -> empty, missing allowedCallers -> target with no callers
//   - refused: invalid JSON, a field of the wrong type, an unknown version or mode
// =============================================================================
#include <gtest/gtest.h>

#include "access_policy.h"

#include <algorithm>
#include <string>

using LogosCore::AccessPolicy;
using LogosCore::parseAccessPolicy;

namespace {

// Find a restriction by target name in a parsed policy, or nullptr.
const LogosCore::AccessRestriction* findTarget(const AccessPolicy& p,
                                               const std::string& target) {
    for (const auto& r : p.restrictions)
        if (r.target == target) return &r;
    return nullptr;
}

bool callersContain(const LogosCore::AccessRestriction& r, const std::string& caller) {
    return std::find(r.allowedCallers.begin(), r.allowedCallers.end(), caller)
           != r.allowedCallers.end();
}

// The exact document basecamp (app/main.cpp) and the logoscore daemon pass.
const char* kProductionPolicy =
    "{\"version\":1,\"mode\":\"enforce\",\"restrictions\":{"
    "\"package_manager\":{\"allowedCallers\":[\"package_manager_ui\"]},"
    "\"package_downloader\":{\"allowedCallers\":[\"package_manager_ui\"]}}}";

} // namespace

// ── Production document ──────────────────────────────────────────────────────

TEST(AccessPolicyParse, ParsesProductionDocument) {
    auto policy = parseAccessPolicy(kProductionPolicy);
    ASSERT_TRUE(policy.has_value());

    EXPECT_EQ(policy->version, 1);
    EXPECT_EQ(policy->mode, "enforce");
    EXPECT_TRUE(policy->enforce());
    ASSERT_EQ(policy->restrictions.size(), 2u);

    const auto* pm = findTarget(*policy, "package_manager");
    ASSERT_NE(pm, nullptr);
    ASSERT_EQ(pm->allowedCallers.size(), 1u);
    EXPECT_TRUE(callersContain(*pm, "package_manager_ui"));

    const auto* pd = findTarget(*policy, "package_downloader");
    ASSERT_NE(pd, nullptr);
    EXPECT_TRUE(callersContain(*pd, "package_manager_ui"));
}

// ── mode semantics ───────────────────────────────────────────────────────────

TEST(AccessPolicyParse, OffModeReportsEnforceFalse) {
    auto policy = parseAccessPolicy(
        "{\"version\":1,\"mode\":\"off\",\"restrictions\":{"
        "\"package_manager\":{\"allowedCallers\":[\"package_manager_ui\"]}}}");
    ASSERT_TRUE(policy.has_value());
    EXPECT_EQ(policy->mode, "off");
    EXPECT_FALSE(policy->enforce());
    // Restrictions still parse — core just won't register them when off.
    EXPECT_EQ(policy->restrictions.size(), 1u);
}

// Detector: a mistyped mode switched enforcement off and the runtime started open.
TEST(AccessPolicyParse, AnUnknownModeIsRefused) {
    for (const char* mode : {"audit", "enforced", "Enforce", ""}) {
        std::string error;
        EXPECT_FALSE(parseAccessPolicy(std::string("{\"version\":1,\"mode\":\"") + mode
                                       + "\",\"restrictions\":{}}", &error).has_value())
            << "'" << mode << "'";
        EXPECT_NE(error.find("mode"), std::string::npos) << error;
    }
}

TEST(AccessPolicyParse, MissingModeIsNotEnforce) {
    auto policy = parseAccessPolicy("{\"version\":1,\"restrictions\":{}}");
    ASSERT_TRUE(policy.has_value());
    EXPECT_FALSE(policy->enforce());
}

// ── Multiple callers per target ──────────────────────────────────────────────

TEST(AccessPolicyParse, ParsesMultipleAllowedCallers) {
    auto policy = parseAccessPolicy(
        "{\"version\":1,\"mode\":\"enforce\",\"restrictions\":{"
        "\"target\":{\"allowedCallers\":[\"a\",\"b\",\"c\"]}}}");
    ASSERT_TRUE(policy.has_value());
    const auto* t = findTarget(*policy, "target");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->allowedCallers.size(), 3u);
    EXPECT_TRUE(callersContain(*t, "a"));
    EXPECT_TRUE(callersContain(*t, "b"));
    EXPECT_TRUE(callersContain(*t, "c"));
}

// ── Tolerant parsing ─────────────────────────────────────────────────────────

TEST(AccessPolicyParse, EmptyRestrictionsObjectYieldsNoRestrictions) {
    auto policy = parseAccessPolicy("{\"version\":1,\"mode\":\"enforce\",\"restrictions\":{}}");
    ASSERT_TRUE(policy.has_value());
    EXPECT_TRUE(policy->enforce());
    EXPECT_TRUE(policy->restrictions.empty());
}

TEST(AccessPolicyParse, MissingRestrictionsKeyYieldsNoRestrictions) {
    auto policy = parseAccessPolicy("{\"version\":1,\"mode\":\"enforce\"}");
    ASSERT_TRUE(policy.has_value());
    EXPECT_TRUE(policy->restrictions.empty());
}

TEST(AccessPolicyParse, TargetWithoutAllowedCallersYieldsEmptyCallerList) {
    // A restricted target with no allowedCallers means "nobody may call it"
    // once enforced — the parser surfaces it as a target with zero callers.
    auto policy = parseAccessPolicy(
        "{\"version\":1,\"mode\":\"enforce\",\"restrictions\":{\"target\":{}}}");
    ASSERT_TRUE(policy.has_value());
    const auto* t = findTarget(*policy, "target");
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(t->allowedCallers.empty());
}

TEST(AccessPolicyParse, UnknownTopLevelKeysAreIgnored) {
    auto policy = parseAccessPolicy(
        "{\"version\":1,\"mode\":\"enforce\",\"futureField\":42,"
        "\"restrictions\":{\"target\":{\"allowedCallers\":[\"a\"],\"note\":\"x\"}}}");
    ASSERT_TRUE(policy.has_value());
    EXPECT_TRUE(policy->enforce());
    const auto* t = findTarget(*policy, "target");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->allowedCallers.size(), 1u);
}

TEST(AccessPolicyParse, NonStringCallersAreSkipped) {
    auto policy = parseAccessPolicy(
        "{\"version\":1,\"mode\":\"enforce\",\"restrictions\":{"
        "\"target\":{\"allowedCallers\":[\"ok\",123,null,\"also_ok\"]}}}");
    ASSERT_TRUE(policy.has_value());
    const auto* t = findTarget(*policy, "target");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->allowedCallers.size(), 2u);
    EXPECT_TRUE(callersContain(*t, "ok"));
    EXPECT_TRUE(callersContain(*t, "also_ok"));
}

// ── Hard failures ────────────────────────────────────────────────────────────

// Detector: a wrong-typed version or mode escaped the parser as an exception.
TEST(AccessPolicyParse, AFieldOfTheWrongTypeIsRefused) {
    for (const char* text : {
             R"({"version":"1","mode":"enforce"})",
             R"({"version":1.5,"mode":"enforce"})",
             R"({"version":1,"mode":1})",
             R"({"version":1,"mode":"enforce","restrictions":[]})",
             R"({"version":1,"mode":"enforce","restrictions":{"t":["a"]}})",
             R"({"version":1,"mode":"enforce","restrictions":{"t":{"allowedCallers":"a"}}})",
             R"({"version":1,"mode":"enforce","restrictions":{"t":{"allowedCallers":{"a":"*"}}}})"}) {
        std::string error;
        EXPECT_FALSE(parseAccessPolicy(text, &error).has_value()) << text;
        EXPECT_FALSE(error.empty()) << text;
    }
}

TEST(AccessPolicyParse, AnUnknownVersionIsRefused) {
    EXPECT_FALSE(parseAccessPolicy(R"({"version":3,"mode":"enforce"})").has_value());
    EXPECT_FALSE(parseAccessPolicy(R"({"version":-1,"mode":"enforce"})").has_value());
    EXPECT_TRUE(parseAccessPolicy(R"({"version":0,"mode":"enforce"})").has_value());
}

TEST(AccessPolicyParse, InvalidJsonReturnsNullopt) {
    EXPECT_FALSE(parseAccessPolicy("{not valid json").has_value());
    EXPECT_FALSE(parseAccessPolicy("").has_value());
    EXPECT_FALSE(parseAccessPolicy("garbage").has_value());
}

TEST(AccessPolicyParse, NonObjectJsonReturnsNullopt) {
    // Valid JSON, but not a policy object.
    EXPECT_FALSE(parseAccessPolicy("[1,2,3]").has_value());
    EXPECT_FALSE(parseAccessPolicy("\"a string\"").has_value());
    EXPECT_FALSE(parseAccessPolicy("42").has_value());
}

TEST(AccessPolicyParse, VersionDefaultsToZeroWhenAbsent) {
    auto policy = parseAccessPolicy("{\"mode\":\"enforce\",\"restrictions\":{}}");
    ASSERT_TRUE(policy.has_value());
    EXPECT_EQ(policy->version, 0);
}
