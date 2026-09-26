#ifdef _WIN32
// Boost.Process uses Boost.Asio, which requires WinSock2 to be selected before
// protocol headers transitively include windows.h.
#include <winsock2.h>
#endif

#include "module_registry.h"
#include "bootstrap_policy.h"
#include "module_state_observer.h"
#include <spdlog/spdlog.h>
#include <cassert>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <boost/dll/runtime_symbol_info.hpp>
#if __has_include(<boost/process/v1.hpp>)
#include <boost/process/v1.hpp>
#else
#include <boost/process.hpp>
#endif
#include <package_manager_lib.h>

namespace logos {

// See the declaration in module_registry.h for the rule and why it lives at
// this trust boundary. Charset already excludes "." and ".."; they are
// re-checked defensively for clarity.
bool isValidModuleName(const std::string& name) {
    if (name.empty() || name.size() > 64)
        return false;
    for (unsigned char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_' || c == '-';
        if (!ok)
            return false;
    }
    if (name == "." || name == "..")
        return false;
    // Core's own key in its token store: a module loaded under it would file
    // its token over core's credential.
    if (name == "core")
        return false;
    return true;
}

}  // namespace logos

static PackageManagerLib& packageManagerInstance() {
    static PackageManagerLib instance;
    return instance;
}

namespace {

namespace fs = std::filesystem;

std::optional<nlohmann::json> readMetadataSidecar(const std::string& modulePath)
{
    const fs::path binary(modulePath);
    const fs::path sidecar = binary.parent_path()
        / (binary.stem().string() + ".metadata.json");
    std::ifstream input(sidecar);
    if (!input) return std::nullopt;
    nlohmann::json value = nlohmann::json::parse(input, nullptr, false);
    if (value.is_discarded() || !value.is_object()) return std::nullopt;
    return value;
}

#ifdef _WIN32
constexpr const char* kExecutableSuffix = ".exe";
#else
constexpr const char* kExecutableSuffix = "";
#endif

#if __has_include(<boost/process/v1.hpp>)
namespace bp = boost::process::v1;
#else
namespace bp = boost::process;
#endif

// Every Qt host that could read a plugin's metadata, in the order tried.
std::vector<fs::path> qtHostCandidates(const std::vector<std::string>& moduleDirs)
{
    std::vector<fs::path> candidates;
    if (const char* configured = std::getenv("LOGOS_HOST_PATH"); configured && *configured) {
        fs::path candidate(configured);
        if (fs::exists(candidate)) candidates.push_back(candidate);
    }
    std::vector<fs::path> directories;
    try {
        directories.push_back(
            fs::path(boost::dll::program_location().parent_path().string()));
    } catch (...) {
    }
    if (!moduleDirs.empty())
        directories.push_back(fs::absolute(fs::path(moduleDirs.front()) / ".." / "bin"));
    for (const auto& directory : directories) {
        for (const char* name : {"logos_host_qt", "logos_host"}) {
            fs::path candidate = (directory / (std::string(name) + kExecutableSuffix)).lexically_normal();
            if (fs::exists(candidate)
                && std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
                candidates.push_back(candidate);
        }
    }
    return candidates;
}

// A host from before --inspect rejects it, and every module without a sidecar
// would read as having no metadata. Asked once per host.
bool canInspect(const fs::path& host)
{
    static std::mutex mutex;
    static std::map<std::string, bool> known;
    std::lock_guard<std::mutex> lock(mutex);
    const auto [entry, fresh] = known.try_emplace(host.string(), false);
    if (!fresh) return entry->second;
    try {
        bp::ipstream output;
        bp::child child(host.string(), "--help", bp::std_out > output, bp::std_err > bp::null);
        std::ostringstream text;
        text << output.rdbuf();
        child.wait();
        entry->second = text.str().find("--inspect") != std::string::npos;
    } catch (const std::exception&) {
    }
    if (!entry->second)
        spdlog::error("{} cannot read Qt plugin metadata (it has no --inspect); "
                      "trying another logos_host_qt", host.string());
    return entry->second;
}

std::optional<nlohmann::json> spawnInspect(
    const std::string& modulePath, const std::vector<std::string>& moduleDirs)
{
    std::optional<fs::path> host;
    for (const auto& candidate : qtHostCandidates(moduleDirs)) {
        if (canInspect(candidate)) {
            host = candidate;
            break;
        }
    }
    if (!host) {
        spdlog::error("No logos_host_qt with --inspect found (set LOGOS_HOST_PATH): "
                      "{} has no metadata sidecar, so it cannot be discovered", modulePath);
        return std::nullopt;
    }
    try {
        bp::ipstream output;
        bp::child child(host->string(), "--inspect", modulePath,
                        bp::std_out > output, bp::std_err > bp::null);
        std::ostringstream text;
        text << output.rdbuf();
        child.wait();
        if (child.exit_code() != 0) return std::nullopt;
        nlohmann::json metadata = nlohmann::json::parse(text.str(), nullptr, false);
        if (metadata.is_discarded() || !metadata.is_object()) return std::nullopt;
        return metadata;
    } catch (const std::exception& exception) {
        spdlog::warn("Qt metadata inspection failed for {}: {}", modulePath,
                     exception.what());
        return std::nullopt;
    }
}

// An --inspect spawn costs 40-100 ms and discovery repeats on every refresh, so
// a binary is asked once until its size or mtime changes.
std::optional<nlohmann::json> inspectQtMetadata(
    const std::string& modulePath, const std::vector<std::string>& moduleDirs)
{
    struct Inspected {
        std::uintmax_t size = 0;
        fs::file_time_type modified;
        nlohmann::json metadata;
    };
    static std::mutex mutex;
    static std::map<std::string, Inspected> cache;
    std::error_code sizeError;
    std::error_code timeError;
    const std::uintmax_t size = fs::file_size(modulePath, sizeError);
    const fs::file_time_type modified = fs::last_write_time(modulePath, timeError);
    const bool identified = !sizeError && !timeError;
    if (identified) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = cache.find(modulePath);
        if (found != cache.end() && found->second.size == size
            && found->second.modified == modified)
            return found->second.metadata;
    }
    auto metadata = spawnInspect(modulePath, moduleDirs);
    if (metadata && identified) {
        std::lock_guard<std::mutex> lock(mutex);
        cache[modulePath] = {size, modified, *metadata};
    }
    return metadata;
}

// Metadata is untrusted input: a field of the wrong type reads as absent
// instead of throwing out of logos_core_start.
std::string stringField(const nlohmann::json& metadata, const char* key)
{
    const auto found = metadata.find(key);
    return found != metadata.end() && found->is_string() ? found->get<std::string>()
                                                         : std::string{};
}

// Same rules as the Qt-era gate: a present but non-string `version` or
// `signer` is a malformed constraint (refused, never unconstrained), and an
// empty name is not a dependency.
std::vector<LogosCore::ModuleDependency> jsonDependencies(
    const nlohmann::json& metadata, const char* field)
{
    std::vector<LogosCore::ModuleDependency> result;
    const auto found = metadata.find(field);
    if (found == metadata.end() || !found->is_array()) return result;
    for (const auto& entry : *found) {
        if (entry.is_string()) {
            if (!entry.get<std::string>().empty())
                result.push_back({entry.get<std::string>(), {}, {}});
        } else if (entry.is_object() && entry.contains("name")
                   && entry["name"].is_string()
                   && !entry["name"].get<std::string>().empty()) {
            std::string range;
            bool malformedConstraint = false;
            if (auto version = entry.find("version"); version != entry.end()) {
                if (version->is_string()) range = version->get<std::string>();
                else malformedConstraint = true;
            }
            std::string signer;
            if (auto value = entry.find("signer"); value != entry.end()) {
                if (value->is_string()) signer = value->get<std::string>();
                else malformedConstraint = true;
            }
            result.push_back({entry["name"].get<std::string>(), range,
                              signer, malformedConstraint});
        }
    }
    return result;
}

std::vector<std::string> dependencyNames(
    const std::vector<LogosCore::ModuleDependency>& deps) {
    std::vector<std::string> names;
    names.reserve(deps.size());
    for (const auto& d : deps) names.push_back(d.name);
    return names;
}

std::vector<LogosCore::ModuleDependency> toDependencyEntries(
    const std::vector<std::string>& names) {
    std::vector<LogosCore::ModuleDependency> deps;
    deps.reserve(names.size());
    for (const auto& n : names) deps.push_back({n, {}, {}});
    return deps;
}

}  // namespace

void ModuleRegistry::setModulesDir(const std::string& dir) {
    std::unique_lock lock(m_mutex);
    m_modulesDirs.clear();
    m_modulesDirs.push_back(dir);
}

void ModuleRegistry::addModulesDir(const std::string& dir) {
    std::unique_lock lock(m_mutex);
    if (std::find(m_modulesDirs.begin(), m_modulesDirs.end(), dir) != m_modulesDirs.end())
        return;
    m_modulesDirs.push_back(dir);
}

std::vector<std::string> ModuleRegistry::modulesDirs() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::string> dirs = m_modulesDirs;
    for (const std::string& dir : m_bundledDirs)
        if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) dirs.push_back(dir);
    return dirs;
}

void ModuleRegistry::setBundledModulesDirs(const std::vector<std::string>& dirs) {
    std::unique_lock lock(m_mutex);
    m_bundledDirs.clear();
    for (const std::string& dir : dirs)
        if (!dir.empty()) m_bundledDirs.push_back(dir);
}

std::vector<std::string> ModuleRegistry::bundledModulesDirs() const {
    std::shared_lock lock(m_mutex);
    return m_bundledDirs;
}

void ModuleRegistry::registerEmbedded(const std::string& name) {
    std::unique_lock lock(m_mutex);
    ModuleInfo& info = m_modules[name];
    info.path = "<embedded>";
    info.embedded = true;
    info.loaded = true;
}

void ModuleRegistry::forgetEmbedded(const std::string& name) {
    std::unique_lock lock(m_mutex);
    auto it = m_modules.find(name);
    if (it != m_modules.end() && it->second.embedded) m_modules.erase(it);
}

bool ModuleRegistry::isBundled(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_modules.find(name);
    return it != m_modules.end() && it->second.bundled;
}

// Once the embedder names its bundled directories, a reserved name comes only
// from them; before that nothing is bundled and names load as they always did.
bool ModuleRegistry::admitsRecordLocked(const std::string& name,
                                        const std::string& modulePath) const {
    if (m_bundledDirs.empty() || !logos::bootstrap::isReservedName(name)
        || isUnderBundledDirLocked(modulePath))
        return true;
    spdlog::error("Refusing module {}: '{}' is reserved for the runtime's bundled modules",
                  modulePath, name);
    return false;
}

// A loaded module keeps the image it runs from until it unloads.
bool ModuleRegistry::keepsLoadedRecordLocked(const ModuleInfo& info, const std::string& name,
                                             const std::string& modulePath) const {
    if (!info.loaded || info.path.empty() || info.path == modulePath) return false;
    spdlog::warn("Module {} is loaded from {}; ignoring {} until it unloads", name, info.path,
                 modulePath);
    return true;
}

// Inside a bundled directory as scanned, or once resolved: a bundle may be a
// symlink farm (a nix buildEnv) whose links point outside it.
bool ModuleRegistry::isUnderBundledDirLocked(const std::string& path) const {
    namespace fs = std::filesystem;
    auto contains = [](fs::path root, const fs::path& file) {
        if (!root.has_filename()) root = root.parent_path();
        const fs::path rel = file.lexically_relative(root);
        return !rel.empty() && *rel.begin() != ".." && *rel.begin() != ".";
    };
    std::error_code error;
    const fs::path given = fs::absolute(fs::path(path), error);
    // `..` would let the text say one directory and the filesystem another.
    const bool lexical = !error && std::none_of(given.begin(), given.end(),
                                                [](const fs::path& part) { return part == ".."; });
    const fs::path resolved = fs::weakly_canonical(fs::path(path), error);
    const bool canonical = !error;
    for (const std::string& dir : m_bundledDirs) {
        std::error_code dirError;
        const fs::path root = fs::absolute(fs::path(dir), dirError).lexically_normal();
        if (lexical && !dirError && contains(root, given.lexically_normal())) return true;
        const fs::path canonicalRoot = fs::weakly_canonical(fs::path(dir), dirError);
        if (canonical && !dirError && contains(canonicalRoot, resolved)) return true;
    }
    return false;
}

void ModuleRegistry::discoverInstalledModules() {
    // ── THE MEMBERSHIP EDGES ─────────────────────────────────────────────────
    //
    // A scan changes the host's SET of known modules in bulk, and is the only
    // place a module can LEAVE it: `absent -> unloaded` on discovery,
    // `unloaded -> absent` on prune. (processModule() below is the other way
    // IN, and emits the discovery edge too.)
    //
    // These edges are why `absent` exists as an event-only state at all —
    // without them a consumer infers membership from package-install events
    // plus a settle timer, which is what basecamp does today.
    //
    // Declared before the lock so the batch dispatches after m_mutex is
    // released — same rule as loadMutex() on the ModuleManager side.
    logos::ScopedModuleStateFlush stateFlusher;

    std::unique_lock lock(m_mutex);

    // Membership before this scan, compared against the results below to derive
    // the edges. Avoids threading reporting down through processModuleInternal,
    // which the single-module path also reaches.
    std::unordered_set<std::string> knownBefore;
    if (logos::ModuleStateObserver::instance().hasSink()) {
        knownBefore.reserve(m_modules.size());
        for (const auto& [name, info] : m_modules)
            knownBefore.insert(name);
    }

    // Bundled directories scan first, so among equals the last-scanned (user)
    // copy still wins, as before; reserved names are settled below.
    std::vector<std::string> scanDirs = m_bundledDirs;
    for (const std::string& dir : m_modulesDirs)
        if (std::find(scanDirs.begin(), scanDirs.end(), dir) == scanDirs.end())
            scanDirs.push_back(dir);

    PackageManagerLib& pm = packageManagerInstance();
    if (!scanDirs.empty()) {
        pm.setEmbeddedModulesDirectory(scanDirs.front());
        for (std::size_t i = 1; i < scanDirs.size(); ++i) {
            pm.addEmbeddedModulesDirectory(scanDirs[i]);
        }
    }

    std::vector<InstalledPackage> modules = pm.getInstalledModules();

    // A reserved name that a user directory also provides resolves to the
    // bundled copy; one no bundled directory provides is refused below.
    if (!m_bundledDirs.empty()) {
        PackageManagerLib bundledPm;
        bundledPm.setEmbeddedModulesDirectory(m_bundledDirs.front());
        for (std::size_t i = 1; i < m_bundledDirs.size(); ++i)
            bundledPm.addEmbeddedModulesDirectory(m_bundledDirs[i]);
        std::unordered_map<std::string, InstalledPackage> bundled;
        for (InstalledPackage& mod : bundledPm.getInstalledModules())
            bundled[mod.name] = std::move(mod);
        for (InstalledPackage& mod : modules) {
            if (!logos::bootstrap::isReservedName(mod.name)
                || isUnderBundledDirLocked(mod.mainFilePath))
                continue;
            if (auto it = bundled.find(mod.name); it != bundled.end()) {
                spdlog::warn("Ignoring {} for reserved module {}: using the bundled {}",
                             mod.mainFilePath, mod.name, it->second.mainFilePath);
                mod = it->second;
            }
        }
    }

    // Collect names seen in this scan. Used after the upsert loop to prune
    // entries for modules whose files disappeared (typical path: the user
    // uninstalls a module — its directory is removed, but without pruning
    // the stale ModuleInfo would stay in m_modules forever and
    // knownModuleNames() (core_service.listModules) would keep returning
    // it, so the UI would never see the uninstall land.
    std::unordered_set<std::string> scannedNames;

    for (const InstalledPackage& mod : modules) {
        if (mod.name.empty() || mod.mainFilePath.empty())
            continue;

        // Bind identity to the TRUSTED package name (mod.name), not the
        // self-asserted name embedded in the plugin binary. processModuleInternal
        // refuses the plugin if its embedded metadata name disagrees, so a
        // package cannot register under a privileged name it doesn't own.
        std::string moduleName = processModuleInternal(mod.mainFilePath, mod.name, mod.version);
        if (moduleName.empty()) {
            spdlog::warn("Failed to process module: {}", mod.mainFilePath);
            continue;
        }
        scannedNames.insert(moduleName);
    }

    // Prune entries that aren't on disk anymore. Preserve currently-loaded
    // modules even if their backing files are gone — the module is still
    // running, and the metadata is still needed by unloadModule / cascade
    // teardown until it exits. The next discovery after that unload will
    // evict the entry.
    std::vector<std::string> toRemove;
    for (const auto& [name, info] : m_modules) {
        if (scannedNames.count(name) == 0 && !info.loaded)
            toRemove.push_back(name);
    }
    for (const std::string& name : toRemove) {
        m_modules.erase(name);
        // Only unloaded entries reach toRemove (a loaded module is preserved
        // even when its files are gone), so it always leaves FROM `unloaded`.
        logos::ModuleStateObserver::instance().record(
            name, logos::module_state::kUnloaded, logos::module_state::kAbsent,
            std::nullopt, std::nullopt, "module files are no longer on disk");
    }

    // Entering the view. Reported after the prune so a scan that both drops and
    // re-adds a name emits the two edges in the order they happened.
    for (const std::string& name : scannedNames) {
        if (knownBefore.count(name) == 0) {
            logos::ModuleStateObserver::instance().record(
                name, logos::module_state::kAbsent, logos::module_state::kUnloaded);
        }
    }

    // Graph has its final shape (upserts + prunes applied). Re-derive
    // dependents so cascade / ModuleManager::getDependents can read them
    // directly from ModuleInfo without re-querying PackageManagerLib.
    recomputeDependentsLocked();
}

std::string ModuleRegistry::processModule(const std::string& modulePath) {
    // The OTHER membership edge: the raw host API, where a module becomes known
    // with no scan. Without this a consumer would be surprised by a
    // `unloaded -> loading` for a module it had never heard of.
    logos::ScopedModuleStateFlush stateFlusher;

    std::unique_lock lock(m_mutex);

    // Whether this UPSERTS or INSERTS is only knowable after the name is
    // resolved from the plugin's metadata, so sample membership first. Skipped
    // when nothing is listening.
    const bool observing = logos::ModuleStateObserver::instance().hasSink();
    std::unordered_set<std::string> knownBefore;
    if (observing) {
        knownBefore.reserve(m_modules.size());
        for (const auto& [n, info] : m_modules)
            knownBefore.insert(n);
    }

    std::string name = processModuleInternal(modulePath);
    // A single module changed, but its new dependency list can invert edges
    // elsewhere in the graph (e.g. an upgrade that drops a dep). Full
    // rebuild is simpler and still O(N * avg_deps) — cheap at module scale.
    recomputeDependentsLocked();

    if (observing && !name.empty() && knownBefore.count(name) == 0) {
        logos::ModuleStateObserver::instance().record(
            name, logos::module_state::kAbsent, logos::module_state::kUnloaded);
    }
    return name;
}

std::string ModuleRegistry::processModuleInternal(const std::string& modulePath,
                                                  const std::string& trustedName,
                                                  const std::string& trustedVersion) {
    auto sidecar = readMetadataSidecar(modulePath);
    // lgpm copies a package over its module directory, so one that ships no
    // sidecar leaves the previous install's behind, describing another binary.
    if (sidecar && !trustedVersion.empty()) {
        const std::string described = stringField(*sidecar, "version");
        if (!described.empty() && described != trustedVersion) {
            spdlog::warn("Ignoring the metadata sidecar of {}: it describes version {}, "
                         "the installed package is {}", modulePath, described, trustedVersion);
            sidecar.reset();
        }
    }
    if (sidecar) {
        const std::string embedded = stringField(*sidecar, "name");
        if (embedded.empty()) {
            spdlog::warn("Module metadata sidecar has no name: {}", modulePath);
            return {};
        }
        const auto transport = sidecar->find("transport");
        const std::string transportName = transport == sidecar->end()
            ? std::string{"qt_remote"} : stringField(*sidecar, "transport");
        if (transportName != "qt_remote" && transportName != "qt_remote_plain") {
            spdlog::warn("Refusing module {}: unsupported transport in its metadata",
                         modulePath);
            return {};
        }
        if (!trustedName.empty() && embedded != trustedName) {
            spdlog::error("Refusing module {}: sidecar name '{}' does not match package name '{}'",
                          modulePath, embedded, trustedName);
            return {};
        }
        const std::string& name = trustedName.empty() ? embedded : trustedName;
        if (!logos::isValidModuleName(name)) {
            spdlog::warn("Rejecting module with invalid name '{}' from {}", name, modulePath);
            return {};
        }
        if (!admitsRecordLocked(name, modulePath)) return {};
        ModuleInfo& info = m_modules[name];
        if (keepsLoadedRecordLocked(info, name, modulePath)) return name;
        info.path = modulePath;
        info.bundled = isUnderBundledDirLocked(modulePath);
        info.format = transportName == "qt_remote_plain" ? "native-cdylib" : "qt-plugin";
        info.metadataJson = sidecar->dump();
        info.version = stringField(*sidecar, "version");
        info.dependencies = jsonDependencies(*sidecar, "dependencies");
        info.optionalDependencies = jsonDependencies(*sidecar, "optional_dependencies");
        return name;
    }

    // Compatibility path for a current qt_remote binary built before module
    // packages began installing sidecars. The same logos_host_qt process that
    // loads it reads Q_PLUGIN_METADATA in metadata-only mode; this parent
    // process remains Qt-free.
    auto metadata = inspectQtMetadata(modulePath, m_modulesDirs);
    if (!metadata || stringField(*metadata, "name").empty()) {
        spdlog::warn("No valid metadata for module: {}", modulePath);
        return {};
    }
    const std::string embedded = stringField(*metadata, "name");

    // When discovery supplies a trusted package name, the plugin's
    // embedded name MUST match it. Otherwise a package installed under an
    // innocuous name could ship a binary claiming a privileged name (e.g.
    // "capability_module") and get wired into that module's token/trust
    // relationships. Refuse the mismatch rather than silently honoring either
    // name. With no trusted name (the raw processModule() host API), fall back
    // to the embedded name as before.
    if (!trustedName.empty() && embedded != trustedName) {
        spdlog::error(
            "Refusing module {}: embedded name '{}' does not match package name '{}'",
            modulePath, embedded, trustedName);
        return {};
    }

    const std::string& name = trustedName.empty() ? embedded : trustedName;

    // The module name comes from untrusted plugin JSON metadata and later
    // becomes the registry map key, the LogosAPI RPC target, and the
    // instance-persistence directory segment. Reject any name that is not a
    // valid module identifier here, at the trust boundary, so a crafted name
    // like "x/../../victim" cannot escape the data dir (CWE-22) or collide
    // with another module's key. isValidModuleName is the single source of
    // truth, so every downstream sink inherits the same guarantee.
    if (!logos::isValidModuleName(name)) {
        spdlog::warn("Rejecting module with invalid name '{}' from {}", name, modulePath);
        return {};
    }

    if (!admitsRecordLocked(name, modulePath)) return {};

    // Update module info in place so re-discovery preserves the loaded flag
    // (and any other state that lives on ModuleInfo).
    ModuleInfo& info = m_modules[name];
    if (keepsLoadedRecordLocked(info, name, modulePath)) return name;
    info.path = modulePath;
    info.bundled = isUnderBundledDirLocked(modulePath);
    info.format = "qt-plugin";
    info.metadataJson = metadata->dump();
    info.version = stringField(*metadata, "version");
    info.dependencies = jsonDependencies(*metadata, "dependencies");
    info.optionalDependencies = jsonDependencies(*metadata, "optional_dependencies");

    return name;
}

bool ModuleRegistry::isKnown(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    return m_modules.count(name) > 0;
}

std::string ModuleRegistry::modulePath(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_modules.find(name);
    return it != m_modules.end() ? it->second.path : std::string{};
}

std::string ModuleRegistry::moduleFormat(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_modules.find(name);
    return it != m_modules.end() ? it->second.format : std::string{};
}

nlohmann::json ModuleRegistry::moduleMetadata(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_modules.find(name);
    if (it == m_modules.end() || it->second.metadataJson.empty())
        return nlohmann::json::object();
    nlohmann::json value = nlohmann::json::parse(
        it->second.metadataJson, nullptr, false);
    return value.is_discarded() ? nlohmann::json::object() : value;
}

nlohmann::json ModuleRegistry::allModulesInfo() const {
    std::shared_lock lock(m_mutex);
    nlohmann::json modules = nlohmann::json::array();
    for (const auto& [name, info] : m_modules) {
        if (info.embedded) continue;
        nlohmann::json entry;
        entry["name"]         = name;
        entry["path"]         = info.path;
        entry["loaded"]       = info.loaded;
        // Unix-seconds timestamp of the current load (0 when not loaded).
        // Callers compute uptime as now - loaded_at while loaded.
        entry["loaded_at"]    = info.loadedAt;
        // Where it runs while loaded: in this process, or a host process of its own.
        entry["placement"]    = !info.loaded || !info.loader ? nlohmann::json(nullptr)
                                : nlohmann::json(info.loader->id() == "inproc" ? "inproc"
                                                                               : "subprocess");
        // Readiness. null (not false) when no watch is armed -- "nobody looked"
        // and "not ready" are different answers.
        entry["published"]    = info.published.has_value()
                                    ? nlohmann::json(*info.published)
                                    : nlohmann::json(nullptr);
        entry["published_at"] = info.publishedAt;
        // Names only: the documented shape of this field (logos_core.h) and of
        // the modules_state snapshot record built from it.
        entry["dependencies"] = dependencyNames(info.dependencies);
        entry["optional_dependencies"] = dependencyNames(info.optionalDependencies);
        entry["optional_dependents"]   = info.optionalDependents;
        entry["dependents"]   = info.dependents;
        // Parse the cached metadata JSON back into structured form. Tolerate a
        // missing/garbled blob by reporting null rather than aborting the call.
        if (info.metadataJson.empty()) {
            entry["metadata"] = nlohmann::json(nullptr);
        } else {
            nlohmann::json meta = nlohmann::json::parse(
                info.metadataJson, nullptr, /*allow_exceptions=*/false);
            entry["metadata"] = meta.is_discarded() ? nlohmann::json(nullptr) : meta;
        }
        modules.push_back(std::move(entry));
    }
    return modules;
}

std::vector<std::string> ModuleRegistry::moduleDependencies(const std::string& name,
                                                            bool recursive) const {
    std::shared_lock lock(m_mutex);
    return moduleDependenciesLocked(name, recursive);
}

std::vector<LogosCore::ModuleDependency>
ModuleRegistry::moduleDependencyEntries(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_modules.find(name);
    return it != m_modules.end() ? it->second.dependencies
                                 : std::vector<LogosCore::ModuleDependency>{};
}

std::vector<std::string>
ModuleRegistry::moduleOptionalDependencies(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_modules.find(name);
    return it != m_modules.end() ? dependencyNames(it->second.optionalDependencies)
                                 : std::vector<std::string>{};
}

std::vector<LogosCore::ModuleDependency>
ModuleRegistry::moduleOptionalDependencyEntries(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_modules.find(name);
    return it != m_modules.end() ? it->second.optionalDependencies
                                 : std::vector<LogosCore::ModuleDependency>{};
}

std::vector<std::string>
ModuleRegistry::moduleOptionalDependents(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_modules.find(name);
    return it != m_modules.end() ? it->second.optionalDependents
                                 : std::vector<std::string>{};
}

std::string ModuleRegistry::moduleVersion(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_modules.find(name);
    return it != m_modules.end() ? it->second.version : std::string{};
}

std::vector<std::string> ModuleRegistry::moduleDependenciesLocked(const std::string& name,
                                                                  bool recursive) const {
    auto it = m_modules.find(name);
    if (it == m_modules.end())
        return {};

    if (!recursive)
        return dependencyNames(it->second.dependencies);

    // BFS over the forward graph. `seen` is pre-seeded with `name` so a
    // dependency cycle that leads back to the target can't append the
    // target to the output — callers treat "transitive deps of X" as
    // "everything needed besides X itself". `out` preserves first-visit
    // order so callers get a stable traversal across diamonds.
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    seen.insert(name);
    std::deque<std::string> queue;
    for (const auto& d : it->second.dependencies) queue.push_back(d.name);
    while (!queue.empty()) {
        std::string current = std::move(queue.front());
        queue.pop_front();
        if (!seen.insert(current).second) continue;
        out.push_back(current);
        auto depIt = m_modules.find(current);
        if (depIt == m_modules.end()) continue;
        for (const auto& d : depIt->second.dependencies) {
            if (seen.count(d.name) == 0) queue.push_back(d.name);
        }
    }
    return out;
}

std::vector<std::string> ModuleRegistry::moduleDependents(const std::string& name,
                                                          bool recursive) const {
    std::shared_lock lock(m_mutex);
    return moduleDependentsLocked(name, recursive);
}

std::vector<std::string> ModuleRegistry::moduleDependentsLocked(const std::string& name,
                                                                bool recursive) const {
    auto it = m_modules.find(name);
    if (it == m_modules.end())
        return {};

    if (!recursive)
        return it->second.dependents;

    // BFS over the reverse graph. Same invariants as the forward walk:
    // `seen` is pre-seeded with `name` so a cyclic edge back to the target
    // doesn't append it to the output; duplicate entries in diamonds are
    // de-duped; `out` preserves first-visit order.
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    seen.insert(name);
    std::deque<std::string> queue(it->second.dependents.begin(),
                                   it->second.dependents.end());
    while (!queue.empty()) {
        std::string current = std::move(queue.front());
        queue.pop_front();
        if (!seen.insert(current).second) continue;
        out.push_back(current);
        auto depIt = m_modules.find(current);
        if (depIt == m_modules.end()) continue;
        for (const std::string& d : depIt->second.dependents) {
            if (seen.count(d) == 0) queue.push_back(d);
        }
    }
    return out;
}

void ModuleRegistry::recomputeDependentsLocked() {
    // Wipe the reverse edges in place — we don't want to reallocate each
    // ModuleInfo, so clear() keeps any existing vector capacity.
    for (auto& [k, v] : m_modules) {
        v.dependents.clear();
        v.optionalDependents.clear();
    }

    // Invert every forward edge, keeping the two sets apart. An entry whose
    // dependency points at an unknown module is silently skipped — we can't
    // register a reverse edge against something we don't track, and logging
    // per-edge here would flood the log during every discovery pass.
    auto invert = [this](const std::string& depender,
                         const std::vector<LogosCore::ModuleDependency>& forward,
                         std::vector<std::string> ModuleInfo::*reverse) {
        for (const auto& entry : forward) {
            auto depIt = m_modules.find(entry.name);
            if (depIt == m_modules.end()) continue;
            auto& deps = depIt->second.*reverse;
            if (std::find(deps.begin(), deps.end(), depender) == deps.end())
                deps.push_back(depender);
        }
    };

    for (const auto& [depender, info] : m_modules) {
        invert(depender, info.dependencies, &ModuleInfo::dependents);
        invert(depender, info.optionalDependencies, &ModuleInfo::optionalDependents);
    }
}

std::vector<std::string> ModuleRegistry::knownModuleNames() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::string> keys;
    keys.reserve(m_modules.size());
    for (const auto& [k, v] : m_modules)
        if (!v.embedded) keys.push_back(k);
    return keys;
}

void ModuleRegistry::registerModule(const std::string& name, const std::string& path,
                                    const std::vector<std::string>& dependencies) {
    std::unique_lock lock(m_mutex);
    ModuleInfo& info = m_modules[name];
    info.path = path;
    // Always assign dependencies (even when empty) and recompute reverse
    // edges. Two reasons we can't gate this on `dependencies.empty()`:
    //   1. Registering "b" with `{}` after an earlier registerDependencies("a",
    //      {"b"}) must give "b" a dependent entry for "a" — the earlier
    //      recompute skipped the unknown edge, and this is the registration
    //      that makes "a → b" visible.
    //   2. Callers need a way to clear forward edges by passing `{}`.
    info.dependencies = toDependencyEntries(dependencies);
    recomputeDependentsLocked();
}

void ModuleRegistry::registerDependencies(const std::string& name, const std::vector<std::string>& dependencies) {
    std::unique_lock lock(m_mutex);
    m_modules[name].dependencies = toDependencyEntries(dependencies);
    // Same reasoning as registerModule: this is a direct graph mutator used
    // by tests. Keep the dependents-consistent-with-dependencies invariant
    // holding across every path that edits forward edges.
    recomputeDependentsLocked();
}

void ModuleRegistry::registerDependencies(
    const std::string& name,
    const std::vector<LogosCore::ModuleDependency>& dependencies) {
    std::unique_lock lock(m_mutex);
    m_modules[name].dependencies = dependencies;
    recomputeDependentsLocked();
}

void ModuleRegistry::registerOptionalDependencies(
    const std::string& name, const std::vector<std::string>& optionalDependencies) {
    std::unique_lock lock(m_mutex);
    m_modules[name].optionalDependencies = toDependencyEntries(optionalDependencies);
    recomputeDependentsLocked();
}

void ModuleRegistry::registerModuleVersion(const std::string& name,
                                           const std::string& version) {
    std::unique_lock lock(m_mutex);
    m_modules[name].version = version;
}

bool ModuleRegistry::isLoaded(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_modules.find(name);
    return it != m_modules.end() && it->second.loaded;
}

// Current wall-clock time in unix seconds. Stamped on load so callers can
// derive a module's uptime; a free function so both markLoaded overloads
// agree on the source.
static int64_t nowUnixSeconds() {
    return static_cast<int64_t>(std::time(nullptr));
}

void ModuleRegistry::markLoaded(const std::string& name) {
    std::unique_lock lock(m_mutex);
    auto& info = m_modules[name];
    info.loaded = true;
    info.loadedAt = nowUnixSeconds();
    info.published.reset();
    info.publishedAt = 0;
    ++info.loadEpoch;
}

void ModuleRegistry::markLoaded(const std::string& name,
                                 std::shared_ptr<LogosCore::ModuleLoader> loader,
                                 LogosCore::LoadedModuleHandle handle) {
    std::unique_lock lock(m_mutex);
    auto& info = m_modules[name];
    info.loaded = true;
    info.loadedAt = nowUnixSeconds();
    info.published.reset();
    info.publishedAt = 0;
    ++info.loadEpoch;
    info.loader = std::move(loader);
    info.handle = std::move(handle);
}

void ModuleRegistry::beginPublishWatch(const std::string& name) {
    std::unique_lock lock(m_mutex);
    auto it = m_modules.find(name);
    if (it != m_modules.end() && !it->second.published.has_value())
        it->second.published = false;
}

bool ModuleRegistry::markPublished(const std::string& name, uint64_t epoch) {
    std::unique_lock lock(m_mutex);
    auto it = m_modules.find(name);
    if (it == m_modules.end()) return false;
    // Reloaded since the watch was armed, or unloaded outright.
    if (it->second.loadEpoch != epoch || !it->second.loaded) return false;
    if (it->second.published == true) return false;
    it->second.published = true;
    it->second.publishedAt = nowUnixSeconds();
    return true;
}

uint64_t ModuleRegistry::loadEpoch(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_modules.find(name);
    return it == m_modules.end() ? 0 : it->second.loadEpoch;
}

std::shared_ptr<LogosCore::ModuleLoader>
ModuleRegistry::loaderFor(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_modules.find(name);
    if (it == m_modules.end()) return nullptr;
    return it->second.loader;
}

void ModuleRegistry::markUnloaded(const std::string& name) {
    std::unique_lock lock(m_mutex);
    auto it = m_modules.find(name);
    if (it != m_modules.end()) {
        it->second.loaded = false;
        it->second.loadedAt = 0;
        it->second.published.reset();
        it->second.publishedAt = 0;
    }
}

std::vector<std::string> ModuleRegistry::loadedModuleNames() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::string> result;
    for (const auto& [k, v] : m_modules) {
        if (v.loaded && !v.embedded)
            result.push_back(k);
    }
    return result;
}

void ModuleRegistry::clearLoaded() {
    std::unique_lock lock(m_mutex);
    for (auto& [k, v] : m_modules)
        v.loaded = false;
}

void ModuleRegistry::clear() {
    std::unique_lock lock(m_mutex);
    m_modulesDirs.clear();
    m_bundledDirs.clear();
    m_modules.clear();
}
