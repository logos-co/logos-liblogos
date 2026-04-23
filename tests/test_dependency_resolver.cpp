// =============================================================================
// Tests for the dependency resolver algorithm exposed via logos_core_resolve_dependencies.
//
// The resolver implements Kahn's topological sort over the registered module
// graph. These tests cover:
//   - Empty / unknown input -> empty result
//   - Single module with no deps
//   - Linear chain (a -> b -> c)
//   - Diamond topology (a -> b,c ; b,c -> d)
//   - Multiple requested roots
//   - Circular dependency (must not hang or crash; partial result expected)
//   - Modules already marked as loaded are still included in the resolved list
//     (the resolver only orders them; the caller decides whether to skip loaded ones)
// =============================================================================
#include <gtest/gtest.h>
#include "logos_core.h"
#include "qt_test_adapter.h"
#include <cstring>
#include <set>
#include <string>
#include <vector>

static void clearModuleState() {
    logos_core_terminate_all();
    logos_core_clear();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int resolvedLen(char** arr) {
    if (!arr) return 0;
    int n = 0;
    while (arr[n]) ++n;
    return n;
}

static std::vector<std::string> resolvedToVec(char** arr) {
    std::vector<std::string> v;
    if (!arr) return v;
    for (int i = 0; arr[i]; ++i)
        v.push_back(arr[i]);
    return v;
}

static void freeResolved(char** arr) {
    if (!arr) return;
    for (int i = 0; arr[i]; ++i)
        delete[] arr[i];
    delete[] arr;
}

class DependencyResolverTest : public ::testing::Test {
protected:
    void SetUp() override { clearModuleState(); }
    void TearDown() override { clearModuleState(); }
};

// ---------------------------------------------------------------------------
// Edge cases: empty / unknown input
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, EmptyInput_ReturnsEmpty) {
    char** result = logos_core_resolve_dependencies(nullptr, 0);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);
    delete[] result;
}

TEST_F(DependencyResolverTest, UnknownModule_ReturnsEmpty) {
    const char* names[] = {"ghost_module"};
    char** result = logos_core_resolve_dependencies(names, 1);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);
    freeResolved(result);
}

TEST_F(DependencyResolverTest, AllUnknown_ReturnsEmpty) {
    const char* names[] = {"alpha", "beta", "gamma"};
    char** result = logos_core_resolve_dependencies(names, 3);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result[0], nullptr);
    freeResolved(result);
}

// ---------------------------------------------------------------------------
// Single module, no dependencies
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, SingleModuleNoDeps_ReturnsSelf) {
    logos_core_register_module("solo", "/path/solo");
    logos_core_register_module_dependencies("solo", nullptr, 0);

    const char* names[] = {"solo"};
    char** result = logos_core_resolve_dependencies(names, 1);

    ASSERT_EQ(resolvedLen(result), 1);
    EXPECT_STREQ(result[0], "solo");
    freeResolved(result);
}

// ---------------------------------------------------------------------------
// Linear chain: a depends on b, b depends on c.
// Expected order: c, b, a.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, LinearChain_CorrectTopologicalOrder) {
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    logos_core_register_module("c", "/c");
    const char* depsA[] = {"b"};
    const char* depsB[] = {"c"};
    logos_core_register_module_dependencies("a", depsA, 1);
    logos_core_register_module_dependencies("b", depsB, 1);
    logos_core_register_module_dependencies("c", nullptr, 0);

    const char* names[] = {"a"};
    char** result = logos_core_resolve_dependencies(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], "c");
    EXPECT_EQ(v[1], "b");
    EXPECT_EQ(v[2], "a");
}

// ---------------------------------------------------------------------------
// Longer chain: d -> c -> b -> a
// Expected order: a, b, c, d
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, FourNodeChain_CorrectOrder) {
    logos_core_register_module("d", "/d");
    logos_core_register_module("c", "/c");
    logos_core_register_module("b", "/b");
    logos_core_register_module("a", "/a");
    const char* depsD[] = {"c"};
    const char* depsC[] = {"b"};
    const char* depsB[] = {"a"};
    logos_core_register_module_dependencies("d", depsD, 1);
    logos_core_register_module_dependencies("c", depsC, 1);
    logos_core_register_module_dependencies("b", depsB, 1);
    logos_core_register_module_dependencies("a", nullptr, 0);

    const char* names[] = {"d"};
    char** result = logos_core_resolve_dependencies(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    ASSERT_EQ(v.size(), 4u);
    EXPECT_EQ(v[0], "a");
    EXPECT_EQ(v[1], "b");
    EXPECT_EQ(v[2], "c");
    EXPECT_EQ(v[3], "d");
}

// ---------------------------------------------------------------------------
// Diamond: a -> b, a -> c; b -> d; c -> d
// Expected: d comes before both b and c, which both come before a.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, DiamondDependency_DSharedRootComesFirst) {
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    logos_core_register_module("c", "/c");
    logos_core_register_module("d", "/d");
    const char* depsA[] = {"b", "c"};
    const char* depsB[] = {"d"};
    const char* depsC[] = {"d"};
    logos_core_register_module_dependencies("a", depsA, 2);
    logos_core_register_module_dependencies("b", depsB, 1);
    logos_core_register_module_dependencies("c", depsC, 1);
    logos_core_register_module_dependencies("d", nullptr, 0);

    const char* names[] = {"a"};
    char** result = logos_core_resolve_dependencies(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    ASSERT_EQ(v.size(), 4u);

    // d must come before b and c; a must come last.
    auto pos = [&](const std::string& name) {
        for (size_t i = 0; i < v.size(); ++i) if (v[i] == name) return (int)i;
        return -1;
    };

    EXPECT_LT(pos("d"), pos("b"));
    EXPECT_LT(pos("d"), pos("c"));
    EXPECT_EQ(v.back(), "a");
}

// ---------------------------------------------------------------------------
// Multiple requested roots: request both a and x (independent trees).
// Both plus their deps should appear in the result.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, MultipleRoots_BothTreesIncluded) {
    // Tree 1: a -> b
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    const char* depsA[] = {"b"};
    logos_core_register_module_dependencies("a", depsA, 1);
    logos_core_register_module_dependencies("b", nullptr, 0);

    // Tree 2: x -> y
    logos_core_register_module("x", "/x");
    logos_core_register_module("y", "/y");
    const char* depsX[] = {"y"};
    logos_core_register_module_dependencies("x", depsX, 1);
    logos_core_register_module_dependencies("y", nullptr, 0);

    const char* names[] = {"a", "x"};
    char** result = logos_core_resolve_dependencies(names, 2);
    auto v = resolvedToVec(result);
    freeResolved(result);

    ASSERT_EQ(v.size(), 4u);

    std::set<std::string> s(v.begin(), v.end());
    EXPECT_TRUE(s.count("a"));
    EXPECT_TRUE(s.count("b"));
    EXPECT_TRUE(s.count("x"));
    EXPECT_TRUE(s.count("y"));

    // Ordering constraints still hold within each tree.
    auto pos = [&](const std::string& name) {
        for (size_t i = 0; i < v.size(); ++i) if (v[i] == name) return (int)i;
        return -1;
    };
    EXPECT_LT(pos("b"), pos("a"));
    EXPECT_LT(pos("y"), pos("x"));
}

// ---------------------------------------------------------------------------
// Shared dependency: two requested roots share a dep.
// The shared dep must appear exactly once in the result.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, SharedDependency_AppearsOnce) {
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    logos_core_register_module("shared", "/shared");
    const char* depsA[] = {"shared"};
    const char* depsB[] = {"shared"};
    logos_core_register_module_dependencies("a", depsA, 1);
    logos_core_register_module_dependencies("b", depsB, 1);
    logos_core_register_module_dependencies("shared", nullptr, 0);

    const char* names[] = {"a", "b"};
    char** result = logos_core_resolve_dependencies(names, 2);
    auto v = resolvedToVec(result);
    freeResolved(result);

    ASSERT_EQ(v.size(), 3u);
    int sharedCount = 0;
    for (const auto& s : v) if (s == "shared") ++sharedCount;
    EXPECT_EQ(sharedCount, 1) << "shared dependency must appear exactly once";
}

// ---------------------------------------------------------------------------
// Mixed known/unknown: requesting both a known and an unknown module.
// Known module and its deps should be resolved; unknown should be silently
// dropped.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, MixedKnownUnknown_UnknownDropped) {
    logos_core_register_module("known", "/known");
    logos_core_register_module_dependencies("known", nullptr, 0);

    const char* names[] = {"known", "unknown_xyz"};
    char** result = logos_core_resolve_dependencies(names, 2);
    auto v = resolvedToVec(result);
    freeResolved(result);

    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], "known");
}

// ---------------------------------------------------------------------------
// Partially unknown dependency: a depends on b, b depends on "missing".
// "missing" is not registered.  The resolver should silently drop "missing"
// and still return a and b in some valid order.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, PartiallyUnknownDep_KnownModulesStillResolved) {
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    const char* depsA[] = {"b"};
    const char* depsB[] = {"missing_dep"};
    logos_core_register_module_dependencies("a", depsA, 1);
    logos_core_register_module_dependencies("b", depsB, 1);

    const char* names[] = {"a"};
    char** result = logos_core_resolve_dependencies(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    // "missing_dep" is unknown so it is skipped; a and b should still appear.
    std::set<std::string> s(v.begin(), v.end());
    EXPECT_TRUE(s.count("a"));
    EXPECT_TRUE(s.count("b"));
    EXPECT_EQ(s.count("missing_dep"), 0u);
}

// ---------------------------------------------------------------------------
// Circular dependency: a -> b -> a.
// Must not hang or crash; the result may be partial.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, CircularDependency_DoesNotHangOrCrash) {
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    const char* depsA[] = {"b"};
    const char* depsB[] = {"a"};
    logos_core_register_module_dependencies("a", depsA, 1);
    logos_core_register_module_dependencies("b", depsB, 1);

    const char* names[] = {"a"};
    // Must return without hanging.
    char** result = logos_core_resolve_dependencies(names, 1);
    // We don't assert a specific result for cycles — just that it doesn't crash.
    freeResolved(result);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Already-loaded modules still appear in the resolved list.
// The resolver is purely structural; the caller decides whether to skip them.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, AlreadyLoadedModule_StillIncludedInResolution) {
    logos_core_register_module("a", "/a");
    logos_core_register_module("b", "/b");
    const char* depsA[] = {"b"};
    logos_core_register_module_dependencies("a", depsA, 1);
    logos_core_register_module_dependencies("b", nullptr, 0);

    // Simulate b already loaded.
    logos_core_mark_module_loaded("b");

    const char* names[] = {"a"};
    char** result = logos_core_resolve_dependencies(names, 1);
    auto v = resolvedToVec(result);
    freeResolved(result);

    // Both a and b must be in the result; the resolver doesn't filter loaded ones.
    std::set<std::string> s(v.begin(), v.end());
    EXPECT_TRUE(s.count("a"));
    EXPECT_TRUE(s.count("b"));
}

// ---------------------------------------------------------------------------
// Duplicate names in the request are collapsed to a single entry.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolverTest, DuplicateNamesInRequest_AppearOnce) {
    logos_core_register_module("a", "/a");
    logos_core_register_module_dependencies("a", nullptr, 0);

    const char* names[] = {"a", "a", "a"};
    char** result = logos_core_resolve_dependencies(names, 3);
    auto v = resolvedToVec(result);
    freeResolved(result);

    int count = 0;
    for (const auto& s : v) if (s == "a") ++count;
    EXPECT_EQ(count, 1) << "duplicate requests should collapse to one entry";
}
