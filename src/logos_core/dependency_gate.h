#ifndef LOGOS_DEPENDENCY_GATE_H
#define LOGOS_DEPENDENCY_GATE_H

#include <functional>
#include <string>
#include <vector>

// Dependency version-range load gate: decoding its inputs, and the decision.
//
// Like the protocol gate this is a COMPATIBILITY control, not a security one:
// range and installed version are both self-asserted plugin metadata. The
// signer pin is carried but NOT enforced — see module_manager.cpp.
//
// It fails CLOSED: a range liblgx cannot parse, or one it cannot evaluate for
// want of a readable installed version, refuses the load. An unenforceable
// constraint is the exact fail-open this gate removes. An entry declaring no
// range is not gated at all, which is every module in the fleet today.
//
// liblgx accepts ^ ~ x-wildcard * and comparator sets, but NOT npm hyphen
// ranges ("1.2.3 - 2.0.0"), and a prerelease ("1.0.0-dev") satisfies no caret
// range — both therefore refuse rather than pass.

namespace LogosCore {

struct ModuleDependency {
    std::string name;
    std::string versionRange;  // empty when the entry declares no range
    std::string signer;        // empty when the entry pins no signer
};

enum class DependencyGateDecision {
    Allow,          // every declared range is satisfied
    Unconstrained,  // no entry declared a range — pre-constraint module
    Refuse,
};

struct DependencyGateResult {
    DependencyGateDecision decision = DependencyGateDecision::Unconstrained;
    // Populated on Refuse only; installedVersion is empty when unreadable.
    std::string dependency;
    std::string range;
    std::string installedVersion;
    std::string reason;
};

// Evaluates the direct edges of one module. Transitive coverage falls out of
// gating every module as it loads. `installedVersionOf` returns "" for a
// dependency that is unknown or carries no version.
DependencyGateResult evaluateDependencyGate(
    const std::vector<ModuleDependency>& dependencies,
    const std::function<std::string(const std::string&)>& installedVersionOf);

// What a module's embedded metadata.json declares about itself, as far as the
// gate is concerned.
struct EmbeddedDeclaration {
    std::string version;
    std::vector<ModuleDependency> dependencies;
};

// Decodes the gate's inputs from the compact metadata blob the registry caches
// at discovery. A dependency entry is either a bare name or
// { name, version, signer }; both declare the same edge, the bare form simply
// constrains nothing. A missing or garbled blob yields empty values — the gate
// treats an unreadable version as unevaluatable, not as a pass.
EmbeddedDeclaration parseEmbeddedDeclaration(const std::string& metadataJson);

} // namespace LogosCore

#endif // LOGOS_DEPENDENCY_GATE_H
