#ifndef BOOTSTRAP_POLICY_H
#define BOOTSTRAP_POLICY_H

// The runtime's own modules and shells, and what each is allowed. Privilege
// comes from this table and from being bundled, never from a name alone:
// a reserved name is refused unless a bundled directory provides it.

#include <string>
#include <vector>

namespace logos::bootstrap {

enum class Placement { Default, InProcess, Subprocess };

struct Row {
    std::string name;
    std::vector<std::string> hostServices; // granted only when bundled
    bool exemptTarget = false;             // no access policy restricts it
    bool trustedCaller = false;            // every target admits it
    Placement placement = Placement::Default;
    bool pinnedInProcess = false;          // no placement policy moves it out
    int teardownRank = 0;                  // higher goes down later
};

// The row for `name` (compared without case), or nullptr.
const Row* rowFor(const std::string& name);

// Names only a bundled directory, or the runtime itself, may provide: the rows,
// the first-party shells and anything starting "logos_". Compared without case.
bool isReservedName(const std::string& name);

const std::vector<std::string>& trustedCallers();
bool isExemptTarget(const std::string& name);

// Empty for a module that is not bundled or has no row.
std::vector<std::string> hostServicesFor(const std::string& name, bool bundled);

// Where `name` runs when the embedder's placement policy does not say.
Placement defaultPlacement(const std::string& name);

// User modules go first (0); capability_module last.
int teardownRank(const std::string& name);

} // namespace logos::bootstrap

#endif // BOOTSTRAP_POLICY_H
