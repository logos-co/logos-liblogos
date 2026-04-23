#include "module_registry.h"
#include <spdlog/spdlog.h>
#include <cassert>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <algorithm>
#include <unordered_set>
#include <module_lib/module_lib.h>
#include <package_manager_lib.h>

static PackageManagerLib& packageManagerInstance() {
    static PackageManagerLib instance;
    return instance;
}

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
    return m_modulesDirs;
}

void ModuleRegistry::discoverInstalledModules() {
    std::unique_lock lock(m_mutex);

    PackageManagerLib& pm = packageManagerInstance();
    if (!m_modulesDirs.empty()) {
        pm.setEmbeddedModulesDirectory(m_modulesDirs.front());
        for (std::size_t i = 1; i < m_modulesDirs.size(); ++i) {
            pm.addEmbeddedModulesDirectory(m_modulesDirs[i]);
        }
    }

    std::vector<InstalledPackage> modules = pm.getInstalledModules();

    // Collect names seen in this scan. Used after the upsert loop to prune
    // entries for modules whose files disappeared (typical path: the user
    // uninstalls a module — its directory is removed, but without pruning
    // the stale ModuleInfo would stay in m_modules forever and
    // knownModuleNames()/`logos_core_get_known_modules` would keep returning
    // it, so the UI would never see the uninstall land.
    std::unordered_set<std::string> scannedNames;

    for (const InstalledPackage& mod : modules) {
        if (mod.name.empty() || mod.mainFilePath.empty())
            continue;

        std::string moduleName = processModuleInternal(mod.mainFilePath);
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
    }

    // Graph has its final shape (upserts + prunes applied). Re-derive
    // dependents so cascade / ModuleManager::getDependents can read them
    // directly from ModuleInfo without re-querying PackageManagerLib.
    recomputeDependentsLocked();
}

std::string ModuleRegistry::processModule(const std::string& modulePath) {
    std::unique_lock lock(m_mutex);
    std::string name = processModuleInternal(modulePath);
    // A single module changed, but its new dependency list can invert edges
    // elsewhere in the graph (e.g. an upgrade that drops a dep). Full
    // rebuild is simpler and still O(N * avg_deps) — cheap at module scale.
    recomputeDependentsLocked();
    return name;
}

std::string ModuleRegistry::processModuleInternal(const std::string& modulePath) {
    std::string name = ModuleLib::LogosModule::getModuleName(modulePath);
    if (name.empty()) {
        spdlog::warn("No valid metadata for module: {}", modulePath);
        return {};
    }

    // Update module info in place so re-discovery preserves the loaded flag
    // (and any other state that lives on ModuleInfo).
    ModuleInfo& info = m_modules[name];
    info.path = modulePath;
    info.dependencies.clear();
    for (const auto& d : ModuleLib::LogosModule::getModuleDependencies(modulePath)) {
        info.dependencies.push_back(d);
    }

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

std::vector<std::string> ModuleRegistry::moduleDependencies(const std::string& name,
                                                            bool recursive) const {
    std::shared_lock lock(m_mutex);
    return moduleDependenciesLocked(name, recursive);
}

std::vector<std::string> ModuleRegistry::moduleDependenciesLocked(const std::string& name,
                                                                  bool recursive) const {
    auto it = m_modules.find(name);
    if (it == m_modules.end())
        return {};

    if (!recursive)
        return it->second.dependencies;

    // BFS over the forward graph. `seen` is pre-seeded with `name` so a
    // dependency cycle that leads back to the target can't append the
    // target to the output — callers treat "transitive deps of X" as
    // "everything needed besides X itself". `out` preserves first-visit
    // order so callers get a stable traversal across diamonds.
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    seen.insert(name);
    std::deque<std::string> queue(it->second.dependencies.begin(),
                                   it->second.dependencies.end());
    while (!queue.empty()) {
        std::string current = std::move(queue.front());
        queue.pop_front();
        if (!seen.insert(current).second) continue;
        out.push_back(current);
        auto depIt = m_modules.find(current);
        if (depIt == m_modules.end()) continue;
        for (const std::string& d : depIt->second.dependencies) {
            if (seen.count(d) == 0) queue.push_back(d);
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
    for (auto& [k, v] : m_modules)
        v.dependents.clear();

    // Invert every forward edge. An entry whose dependency points at an
    // unknown module is silently skipped — we can't register a reverse
    // edge against something we don't track, and logging per-edge here
    // would flood the log during every discovery pass.
    for (const auto& [depender, info] : m_modules) {
        for (const std::string& dep : info.dependencies) {
            auto depIt = m_modules.find(dep);
            if (depIt == m_modules.end()) continue;
            auto& deps = depIt->second.dependents;
            if (std::find(deps.begin(), deps.end(), depender) == deps.end())
                deps.push_back(depender);
        }
    }
}

std::vector<std::string> ModuleRegistry::knownModuleNames() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::string> keys;
    keys.reserve(m_modules.size());
    for (const auto& [k, v] : m_modules)
        keys.push_back(k);
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
    info.dependencies = dependencies;
    recomputeDependentsLocked();
}

void ModuleRegistry::registerDependencies(const std::string& name, const std::vector<std::string>& dependencies) {
    std::unique_lock lock(m_mutex);
    m_modules[name].dependencies = dependencies;
    // Same reasoning as registerModule: this is a direct graph mutator used
    // by tests. Keep the dependents-consistent-with-dependencies invariant
    // holding across every path that edits forward edges.
    recomputeDependentsLocked();
}

bool ModuleRegistry::isLoaded(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_modules.find(name);
    return it != m_modules.end() && it->second.loaded;
}

void ModuleRegistry::markLoaded(const std::string& name) {
    std::unique_lock lock(m_mutex);
    m_modules[name].loaded = true;
}

void ModuleRegistry::markLoaded(const std::string& name,
                                 std::shared_ptr<LogosCore::ModuleRuntime> runtime,
                                 LogosCore::LoadedModuleHandle handle) {
    std::unique_lock lock(m_mutex);
    auto& info = m_modules[name];
    info.loaded  = true;
    info.runtime = std::move(runtime);
    info.handle  = std::move(handle);
}

std::shared_ptr<LogosCore::ModuleRuntime>
ModuleRegistry::runtimeFor(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_modules.find(name);
    if (it == m_modules.end()) return nullptr;
    return it->second.runtime;
}

void ModuleRegistry::markUnloaded(const std::string& name) {
    std::unique_lock lock(m_mutex);
    auto it = m_modules.find(name);
    if (it != m_modules.end())
        it->second.loaded = false;
}

std::vector<std::string> ModuleRegistry::loadedModuleNames() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::string> result;
    for (const auto& [k, v] : m_modules) {
        if (v.loaded)
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
    m_modules.clear();
}
