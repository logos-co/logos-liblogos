#include <gtest/gtest.h>

#include "logos_core/dependency_gate.h"

#include <map>
#include <string>

// The gate's decision table. Ranges are evaluated by liblgx's semver, so these
// also pin the npm dialect the manifests already use.

using LogosCore::DependencyGateDecision;
using LogosCore::evaluateDependencyGate;
using LogosCore::ModuleDependency;

namespace {

// Installed versions keyed by module name; a name that is absent stands for a
// dependency with no readable version.
auto installed(std::map<std::string, std::string> versions)
{
    return [versions = std::move(versions)](const std::string& name) {
        auto it = versions.find(name);
        return it != versions.end() ? it->second : std::string{};
    };
}

}  // namespace

// The string form of a dependency entry, and the object form that names no
// version, both declare an edge and constrain nothing.
TEST(DependencyGate, NoRangeIsUnconstrained)
{
    const std::vector<ModuleDependency> deps = {
        {"chat_module", "", ""},
        {"waku_module", "", "did:jwk:abc"},
    };
    const auto r = evaluateDependencyGate(deps, installed({{"chat_module", "1.0.0"}}));
    EXPECT_EQ(r.decision, DependencyGateDecision::Unconstrained);
    EXPECT_TRUE(r.reason.empty());
}

TEST(DependencyGate, SatisfiedRangeAllows)
{
    const std::vector<ModuleDependency> deps = {{"chat_module", "^2.0.0", ""}};
    const auto r = evaluateDependencyGate(deps, installed({{"chat_module", "2.1.0"}}));
    EXPECT_EQ(r.decision, DependencyGateDecision::Allow);
    EXPECT_TRUE(r.reason.empty());
}

TEST(DependencyGate, UnsatisfiedRangeRefuses)
{
    const std::vector<ModuleDependency> deps = {{"chat_module", "^2.0.0", ""}};
    const auto r = evaluateDependencyGate(deps, installed({{"chat_module", "1.0.0"}}));
    ASSERT_EQ(r.decision, DependencyGateDecision::Refuse);
    EXPECT_EQ(r.dependency, "chat_module");
    EXPECT_EQ(r.range, "^2.0.0");
    EXPECT_EQ(r.installedVersion, "1.0.0");
    EXPECT_NE(r.reason.find("^2.0.0"), std::string::npos);
    EXPECT_NE(r.reason.find("1.0.0"), std::string::npos);
}

// Fail closed: a constraint that cannot be parsed is the exact fail-open the
// gate exists to remove, so it refuses rather than being ignored.
TEST(DependencyGate, MalformedRangeRefuses)
{
    for (const char* bad : {"not a range", "^^1.0.0", "garbage!!"}) {
        const std::vector<ModuleDependency> deps = {{"chat_module", bad, ""}};
        const auto r = evaluateDependencyGate(deps, installed({{"chat_module", "1.0.0"}}));
        EXPECT_EQ(r.decision, DependencyGateDecision::Refuse) << bad;
        EXPECT_NE(r.reason.find("unparseable"), std::string::npos) << bad;
    }
}

// The dialect liblgx actually implements, pinned because the refusals surprise:
// npm hyphen ranges are not supported, and a prerelease satisfies no caret
// range. Both fail closed, so a module author sees a refusal, not a silent pass.
TEST(DependencyGate, RangeDialectLiblgxImplements)
{
    for (const char* ok : {"^2.0.0", "~2.1.0", "2.x", "*", "2.1.0", ">=2.0.0 <3.0.0"}) {
        const std::vector<ModuleDependency> deps = {{"chat_module", ok, ""}};
        EXPECT_EQ(evaluateDependencyGate(deps, installed({{"chat_module", "2.1.0"}})).decision,
                  DependencyGateDecision::Allow) << ok;
    }
    const std::vector<ModuleDependency> hyphen = {{"chat_module", "1.2.3 - 2.0.0", ""}};
    EXPECT_EQ(evaluateDependencyGate(hyphen, installed({{"chat_module", "1.5.0"}})).decision,
              DependencyGateDecision::Refuse);

    const std::vector<ModuleDependency> caret = {{"chat_module", "^1.0.0", ""}};
    EXPECT_EQ(evaluateDependencyGate(caret, installed({{"chat_module", "1.0.0-dev"}})).decision,
              DependencyGateDecision::Refuse);
}

// Same reason: a range declared against a dependency whose version cannot be
// read is unevaluatable, not satisfied.
TEST(DependencyGate, UnreadableInstalledVersionRefuses)
{
    const std::vector<ModuleDependency> deps = {{"chat_module", "^2.0.0", ""}};
    const auto r = evaluateDependencyGate(deps, installed({}));
    ASSERT_EQ(r.decision, DependencyGateDecision::Refuse);
    EXPECT_TRUE(r.installedVersion.empty());
    EXPECT_NE(r.reason.find("(unknown)"), std::string::npos);
}

TEST(DependencyGate, UnparseableInstalledVersionRefuses)
{
    const std::vector<ModuleDependency> deps = {{"chat_module", "^2.0.0", ""}};
    const auto r = evaluateDependencyGate(deps, installed({{"chat_module", "banana"}}));
    EXPECT_EQ(r.decision, DependencyGateDecision::Refuse);
}

// A mixed list refuses on the offending edge, and names that edge rather than
// the first one it walked.
TEST(DependencyGate, ReportsTheOffendingEdge)
{
    const std::vector<ModuleDependency> deps = {
        {"waku_module", "", ""},
        {"chat_module", ">=1.0.0", ""},
        {"storage_module", "~3.2.0", ""},
    };
    const auto r = evaluateDependencyGate(
        deps, installed({{"chat_module", "1.4.0"}, {"storage_module", "3.5.0"}}));
    ASSERT_EQ(r.decision, DependencyGateDecision::Refuse);
    EXPECT_EQ(r.dependency, "storage_module");
    EXPECT_EQ(r.installedVersion, "3.5.0");
}

// A signer pin alone is carried, never enforced — enforcement needs evidence
// lgpm does not persist. See the note at the enforcement point.
TEST(DependencyGate, SignerPinAloneIsNotEnforced)
{
    const std::vector<ModuleDependency> deps = {{"chat_module", "", "did:jwk:whoever"}};
    EXPECT_EQ(evaluateDependencyGate(deps, installed({{"chat_module", "1.0.0"}})).decision,
              DependencyGateDecision::Unconstrained);
}

TEST(DependencyGate, NoDependenciesIsUnconstrained)
{
    EXPECT_EQ(evaluateDependencyGate({}, installed({})).decision,
              DependencyGateDecision::Unconstrained);
}

// =============================================================================
// Decoding the gate's inputs from a module's embedded metadata blob. This is
// where the constraint used to be discarded, so the object form is the case
// that matters.
// =============================================================================

using LogosCore::parseEmbeddedDeclaration;

TEST(EmbeddedDeclaration, ObjectFormKeepsRangeAndSigner)
{
    const auto d = parseEmbeddedDeclaration(R"({
        "version": "2.3.4",
        "dependencies": [
            {"name": "chat_module", "version": "^2.0.0", "signer": "did:jwk:whoever"}
        ]
    })");
    EXPECT_EQ(d.version, "2.3.4");
    ASSERT_EQ(d.dependencies.size(), 1u);
    EXPECT_EQ(d.dependencies[0].name, "chat_module");
    EXPECT_EQ(d.dependencies[0].versionRange, "^2.0.0");
    EXPECT_EQ(d.dependencies[0].signer, "did:jwk:whoever");
}

// The form every module in the fleet uses today: an edge that constrains
// nothing, so the gate stays out of the way.
TEST(EmbeddedDeclaration, BareNameFormConstrainsNothing)
{
    const auto d = parseEmbeddedDeclaration(
        R"({"version":"1.0.0","dependencies":["chat_module","storage_module"]})");
    ASSERT_EQ(d.dependencies.size(), 2u);
    EXPECT_EQ(d.dependencies[0].name, "chat_module");
    EXPECT_TRUE(d.dependencies[0].versionRange.empty());
    EXPECT_TRUE(d.dependencies[1].signer.empty());
    EXPECT_EQ(evaluateDependencyGate(d.dependencies,
                                     installed({{"chat_module", "1.0.0"},
                                                {"storage_module", "1.0.0"}}))
                  .decision,
              DependencyGateDecision::Unconstrained);
}

TEST(EmbeddedDeclaration, MixedFormsInOneArray)
{
    const auto d = parseEmbeddedDeclaration(
        R"({"dependencies":["a",{"name":"b","version":"~1.2.0"}]})");
    ASSERT_EQ(d.dependencies.size(), 2u);
    EXPECT_TRUE(d.dependencies[0].versionRange.empty());
    EXPECT_EQ(d.dependencies[1].versionRange, "~1.2.0");
    EXPECT_TRUE(d.version.empty());
}

// A blob that is absent, unparseable, or wrongly typed must decode to nothing
// rather than throwing or inventing an edge.
TEST(EmbeddedDeclaration, UnreadableBlobsDecodeToNothing)
{
    for (const std::string& blob : {std::string(""), std::string("{not json"),
                                    std::string("[]"), std::string("null"),
                                    std::string(R"({"dependencies":"chat_module"})"),
                                    std::string(R"({"version":7})")}) {
        const auto d = parseEmbeddedDeclaration(blob);
        EXPECT_TRUE(d.version.empty()) << blob;
        EXPECT_TRUE(d.dependencies.empty()) << blob;
    }
}

// A nameless entry is not an edge; keeping it would key the graph on "".
TEST(EmbeddedDeclaration, NamelessEntriesAreDropped)
{
    const auto d = parseEmbeddedDeclaration(
        R"({"dependencies":[{"version":"^1.0.0"},"",{"name":"b"}]})");
    ASSERT_EQ(d.dependencies.size(), 1u);
    EXPECT_EQ(d.dependencies[0].name, "b");
}

// The end-to-end shape the gate exists for: a declared "^2.0.0" decoded off the
// blob refuses against an installed 1.0.0 and allows against 2.1.0.
TEST(EmbeddedDeclaration, DecodedRangeDrivesTheGate)
{
    const auto d = parseEmbeddedDeclaration(
        R"({"dependencies":[{"name":"chat_module","version":"^2.0.0"}]})");
    EXPECT_EQ(evaluateDependencyGate(d.dependencies, installed({{"chat_module", "1.0.0"}})).decision,
              DependencyGateDecision::Refuse);
    EXPECT_EQ(evaluateDependencyGate(d.dependencies, installed({{"chat_module", "2.1.0"}})).decision,
              DependencyGateDecision::Allow);
}
