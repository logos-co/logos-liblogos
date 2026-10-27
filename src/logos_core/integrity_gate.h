#ifndef LOGOS_INTEGRITY_GATE_H
#define LOGOS_INTEGRITY_GATE_H

#include <string>
#include <vector>

// Installed-module integrity load gate: are a module's files still the ones
// the package manager installed? Re-hashes the install directory against the
// manifest.json lgpm wrote there (liblgx's installed-tree check).
//
// Only modules installed under one of the frontend's VERIFIED directories are
// checked -- the user modules directory the package manager installs into.
// Bundled modules and directories a developer passes in were never installed,
// so there is nothing to compare them against.
//
// It fails CLOSED: a missing manifest or variant file, a manifest without
// hashes, or a tree that cannot be read all refuse. lgpm writes all of these on
// every install, so their absence means the directory is not what it installed.
//
// Like the other gates this is not a defence against an attacker who can
// already write the user's files: they can rewrite the manifest too. It
// catches corruption and edits made after install.

namespace LogosCore {

enum class IntegrityGateDecision {
    NotChecked,  // not installed under a verified directory
    Allow,
    Refuse,
};

struct IntegrityGateResult {
    IntegrityGateDecision decision = IntegrityGateDecision::NotChecked;
    std::string reason;  // populated on Refuse only
};

IntegrityGateResult evaluateIntegrityGate(const std::string& installDir,
                                          const std::vector<std::string>& verifiedDirs);

// The check itself, for a directory the caller already knows the package
// manager installed. Allow or Refuse, never NotChecked.
IntegrityGateResult checkInstalledTree(const std::string& installDir);

} // namespace LogosCore

#endif // LOGOS_INTEGRITY_GATE_H
