// =============================================================================
// Dedicated tests for DependencyResolver::resolve
//
// These tests establish the behavioural contract of the topological sort used
// to compute plugin load order. They are intentionally decoupled from the
// PluginRegistry so the resolver can be swapped (e.g. to std containers) with
// the same expectations intact.
// =============================================================================
#include <gtest/gtest.h>
#include "dependency_resolver.h"
#include <QHash>
#include <QStringList>
#include <QSet>
#include <algorithm>

namespace {

// Simple in-memory dependency graph used by every test in this file.
struct Graph {
    QHash<QString, QStringList> deps;

    bool isKnown(const QString& name) const { return deps.contains(name); }
    QStringList getDependencies(const QString& name) const { return deps.value(name); }

    DependencyResolver::IsKnownFn isKnownFn() const {
        return [this](const QString& n) { return this->isKnown(n); };
    }
    DependencyResolver::GetDependenciesFn getDepsFn() const {
        return [this](const QString& n) { return this->getDependencies(n); };
    }
};

// Returns the 0-based index of `name` in `order`, or -1 if absent. Used to
// assert "dep comes before dependent" without pinning an exact ordering for
// siblings whose relative order is unspecified.
int indexOf(const QStringList& order, const QString& name) {
    return order.indexOf(name);
}

} // namespace

// =============================================================================
// Empty / trivial cases
// =============================================================================

TEST(DependencyResolver, EmptyRequest_ReturnsEmpty) {
    Graph g;
    QStringList result = DependencyResolver::resolve({}, g.isKnownFn(), g.getDepsFn());
    EXPECT_TRUE(result.isEmpty());
}

TEST(DependencyResolver, UnknownModule_IsSkipped) {
    Graph g;
    QStringList result = DependencyResolver::resolve(
        QStringList{"ghost"}, g.isKnownFn(), g.getDepsFn());
    EXPECT_TRUE(result.isEmpty());
}

TEST(DependencyResolver, SingleModuleNoDeps_ReturnsItself) {
    Graph g;
    g.deps["a"] = {};
    QStringList result = DependencyResolver::resolve(
        QStringList{"a"}, g.isKnownFn(), g.getDepsFn());
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result.first().toStdString(), "a");
}

// =============================================================================
// Linear chains
// =============================================================================

TEST(DependencyResolver, LinearChain_DepsBeforeDependents) {
    Graph g;
    g.deps["a"] = {"b"};
    g.deps["b"] = {"c"};
    g.deps["c"] = {};

    QStringList result = DependencyResolver::resolve(
        QStringList{"a"}, g.isKnownFn(), g.getDepsFn());

    ASSERT_EQ(result.size(), 3);
    EXPECT_LT(indexOf(result, "c"), indexOf(result, "b"));
    EXPECT_LT(indexOf(result, "b"), indexOf(result, "a"));
}

TEST(DependencyResolver, DeepChain_FiveLevels) {
    Graph g;
    g.deps["level0"] = {"level1"};
    g.deps["level1"] = {"level2"};
    g.deps["level2"] = {"level3"};
    g.deps["level3"] = {"level4"};
    g.deps["level4"] = {};

    QStringList result = DependencyResolver::resolve(
        QStringList{"level0"}, g.isKnownFn(), g.getDepsFn());

    ASSERT_EQ(result.size(), 5);
    for (int i = 0; i < 4; ++i) {
        int deeper = indexOf(result, QString("level%1").arg(4 - i));
        int shallower = indexOf(result, QString("level%1").arg(3 - i));
        EXPECT_LT(deeper, shallower)
            << "level" << (4 - i) << " must load before level" << (3 - i);
    }
}

// =============================================================================
// Diamond dependency
//
//       a
//      / \
//     b   c
//      \ /
//       d
// =============================================================================

TEST(DependencyResolver, Diamond_DLoadsBeforeEverything) {
    Graph g;
    g.deps["a"] = {"b", "c"};
    g.deps["b"] = {"d"};
    g.deps["c"] = {"d"};
    g.deps["d"] = {};

    QStringList result = DependencyResolver::resolve(
        QStringList{"a"}, g.isKnownFn(), g.getDepsFn());

    ASSERT_EQ(result.size(), 4);
    int idxA = indexOf(result, "a");
    int idxB = indexOf(result, "b");
    int idxC = indexOf(result, "c");
    int idxD = indexOf(result, "d");

    ASSERT_GE(idxA, 0);
    ASSERT_GE(idxB, 0);
    ASSERT_GE(idxC, 0);
    ASSERT_GE(idxD, 0);

    EXPECT_LT(idxD, idxB) << "d must load before b";
    EXPECT_LT(idxD, idxC) << "d must load before c";
    EXPECT_LT(idxB, idxA) << "b must load before a";
    EXPECT_LT(idxC, idxA) << "c must load before a";
}

TEST(DependencyResolver, Diamond_NoDuplicates) {
    Graph g;
    g.deps["a"] = {"b", "c"};
    g.deps["b"] = {"d"};
    g.deps["c"] = {"d"};
    g.deps["d"] = {};

    QStringList result = DependencyResolver::resolve(
        QStringList{"a"}, g.isKnownFn(), g.getDepsFn());

    QSet<QString> unique(result.begin(), result.end());
    EXPECT_EQ(result.size(), unique.size())
        << "d is shared by b and c but must only appear once in result";
}

// =============================================================================
// Independent modules — no cross-deps
// =============================================================================

TEST(DependencyResolver, IndependentModules_AllAppearExactlyOnce) {
    Graph g;
    g.deps["a"] = {};
    g.deps["b"] = {};
    g.deps["c"] = {};

    QStringList result = DependencyResolver::resolve(
        QStringList{"a", "b", "c"}, g.isKnownFn(), g.getDepsFn());

    ASSERT_EQ(result.size(), 3);
    EXPECT_TRUE(result.contains("a"));
    EXPECT_TRUE(result.contains("b"));
    EXPECT_TRUE(result.contains("c"));
}

// =============================================================================
// Transitive pulls — requesting a leaf also pulls its parents via reverse
// paths? No — a leaf has no deps, but if we request a middle module we should
// still get its own deps but NOT anything that depends on it.
// =============================================================================

TEST(DependencyResolver, RequestingMiddle_DoesNotPullUnrelatedParents) {
    Graph g;
    g.deps["parent"] = {"middle"};
    g.deps["middle"] = {"child"};
    g.deps["child"] = {};

    QStringList result = DependencyResolver::resolve(
        QStringList{"middle"}, g.isKnownFn(), g.getDepsFn());

    ASSERT_EQ(result.size(), 2);
    EXPECT_TRUE(result.contains("middle"));
    EXPECT_TRUE(result.contains("child"));
    EXPECT_FALSE(result.contains("parent"))
        << "parent depends on middle but was not requested; should not appear";
}

// =============================================================================
// Partial failure — requested list contains both known and unknown
// =============================================================================

TEST(DependencyResolver, PartiallyKnown_KnownAreStillResolved) {
    Graph g;
    g.deps["real"] = {};

    QStringList result = DependencyResolver::resolve(
        QStringList{"real", "ghost"}, g.isKnownFn(), g.getDepsFn());

    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result.first().toStdString(), "real");
}

TEST(DependencyResolver, MissingTransitiveDep_OtherDepsStillLoad) {
    Graph g;
    g.deps["a"] = {"b", "missing"};
    g.deps["b"] = {};
    // "missing" not in graph

    QStringList result = DependencyResolver::resolve(
        QStringList{"a"}, g.isKnownFn(), g.getDepsFn());

    EXPECT_TRUE(result.contains("a"));
    EXPECT_TRUE(result.contains("b"));
    EXPECT_FALSE(result.contains("missing"));
}

// =============================================================================
// Cycle detection
// =============================================================================

TEST(DependencyResolver, SelfLoop_IsHandledWithoutHang) {
    Graph g;
    g.deps["loop"] = {"loop"};

    // Expected behaviour: result will not contain "loop" because in-degree is 1
    // and there is no zero-in-degree node to start Kahn's algorithm from. The
    // current implementation logs a "Circular dependency detected" warning.
    // The critical property under test is that this does not hang.
    QStringList result = DependencyResolver::resolve(
        QStringList{"loop"}, g.isKnownFn(), g.getDepsFn());

    EXPECT_FALSE(result.contains("loop"))
        << "a self-looping module cannot satisfy topological sort";
}

TEST(DependencyResolver, TwoNodeCycle_IsNotIncludedInOutput) {
    Graph g;
    g.deps["a"] = {"b"};
    g.deps["b"] = {"a"};

    QStringList result = DependencyResolver::resolve(
        QStringList{"a"}, g.isKnownFn(), g.getDepsFn());

    // Kahn's algorithm cannot order a cycle — neither node should appear in
    // the final sorted output.
    EXPECT_EQ(result.size(), 0)
        << "a <-> b cycle must be excluded from topological output";
}

TEST(DependencyResolver, CycleDoesNotPreventUnrelatedModulesFromLoading) {
    Graph g;
    g.deps["a"] = {"b"};
    g.deps["b"] = {"a"};
    g.deps["independent"] = {};

    QStringList result = DependencyResolver::resolve(
        QStringList{"a", "independent"}, g.isKnownFn(), g.getDepsFn());

    EXPECT_TRUE(result.contains("independent"))
        << "a cycle in one part of the graph must not block unrelated modules";
}

// =============================================================================
// Duplicate requests — requesting the same module twice must not duplicate
// output (regression guard: any port to std::unordered_set must retain this).
// =============================================================================

TEST(DependencyResolver, DuplicateRequest_IsDeduplicated) {
    Graph g;
    g.deps["a"] = {};

    QStringList result = DependencyResolver::resolve(
        QStringList{"a", "a", "a"}, g.isKnownFn(), g.getDepsFn());

    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result.first().toStdString(), "a");
}

// =============================================================================
// Empty-string dep entries (can appear in malformed metadata JSON) — must be
// silently skipped rather than treated as a "no name" module.
// =============================================================================

TEST(DependencyResolver, EmptyStringDep_IsIgnored) {
    Graph g;
    g.deps["a"] = {"", "b", ""};
    g.deps["b"] = {};

    QStringList result = DependencyResolver::resolve(
        QStringList{"a"}, g.isKnownFn(), g.getDepsFn());

    ASSERT_EQ(result.size(), 2);
    EXPECT_TRUE(result.contains("a"));
    EXPECT_TRUE(result.contains("b"));
    EXPECT_FALSE(result.contains(""));
}

// =============================================================================
// Ordering determinism — running the same input twice must produce the same
// output. This is the minimum guarantee any replacement implementation (std
// containers) must preserve; otherwise UI code that displays modules in load
// order will flicker between runs.
// =============================================================================

TEST(DependencyResolver, RepeatedRuns_ProduceSameOrder) {
    Graph g;
    g.deps["top"] = {"mid_a", "mid_b"};
    g.deps["mid_a"] = {"leaf"};
    g.deps["mid_b"] = {"leaf"};
    g.deps["leaf"] = {};

    QStringList first = DependencyResolver::resolve(
        QStringList{"top"}, g.isKnownFn(), g.getDepsFn());
    QStringList second = DependencyResolver::resolve(
        QStringList{"top"}, g.isKnownFn(), g.getDepsFn());

    EXPECT_EQ(first, second)
        << "topological sort must be deterministic across invocations";
}
