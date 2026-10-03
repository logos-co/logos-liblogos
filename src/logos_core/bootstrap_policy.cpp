#include "bootstrap_policy.h"

#include <algorithm>
#include <cctype>

namespace logos::bootstrap {
namespace {

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// `core` is the engine's legacy principal: it has no module, only the name.
const std::vector<Row>& rows()
{
    static const std::vector<Row> table = {
        {"core", {}, true, true, Placement::Default, false, 0},
        {"core_service", {}, true, true, Placement::Default, false, 3},
        {"capability_module", {"token_registry", "token_delivery"}, true, false,
         Placement::InProcess, true, 4},
        {"modules_state", {}, false, false, Placement::InProcess, false, 2},
        {"package_manager", {}, false, false, Placement::InProcess, false, 1},
        {"package_downloader", {}, false, false, Placement::InProcess, false, 1},
    };
    return table;
}

const std::vector<std::string>& shellNames()
{
    static const std::vector<std::string> names = {"logoscore", "basecamp", "standalone",
                                                   "module_viewer"};
    return names;
}

} // namespace

const Row* rowFor(const std::string& name)
{
    const std::string key = lower(name);
    for (const Row& row : rows())
        if (row.name == key) return &row;
    return nullptr;
}

bool isReservedName(const std::string& name)
{
    const std::string key = lower(name);
    if (key.rfind("logos_", 0) == 0) return true;
    if (rowFor(key)) return true;
    return std::find(shellNames().begin(), shellNames().end(), key) != shellNames().end();
}

const std::vector<std::string>& trustedCallers()
{
    static const std::vector<std::string> callers = [] {
        std::vector<std::string> names;
        for (const Row& row : rows())
            if (row.trustedCaller) names.push_back(row.name);
        return names;
    }();
    return callers;
}

bool isExemptTarget(const std::string& name)
{
    const Row* row = rowFor(name);
    return row && row->exemptTarget;
}

std::vector<std::string> hostServicesFor(const std::string& name, bool bundled)
{
    const Row* row = rowFor(name);
    return row && bundled ? row->hostServices : std::vector<std::string>{};
}

Placement defaultPlacement(const std::string& name)
{
    const Row* row = rowFor(name);
    return row ? row->placement : Placement::Default;
}

int teardownRank(const std::string& name)
{
    const Row* row = rowFor(name);
    return row ? row->teardownRank : 0;
}

} // namespace logos::bootstrap
