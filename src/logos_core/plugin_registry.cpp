#include "plugin_registry.h"
#include <spdlog/spdlog.h>
#include <cassert>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <algorithm>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <module_lib/module_lib.h>
#include <package_manager_lib.h>

static PackageManagerLib& packageManagerInstance() {
    static PackageManagerLib instance;
    return instance;
}

void PluginRegistry::setPluginsDir(const std::string& dir) {
    std::unique_lock lock(m_mutex);
    m_pluginsDirs.clear();
    m_pluginsDirs.push_back(dir);
}

void PluginRegistry::addPluginsDir(const std::string& dir) {
    std::unique_lock lock(m_mutex);
    if (std::find(m_pluginsDirs.begin(), m_pluginsDirs.end(), dir) != m_pluginsDirs.end())
        return;
    m_pluginsDirs.push_back(dir);
}

std::vector<std::string> PluginRegistry::pluginsDirs() const {
    std::shared_lock lock(m_mutex);
    return m_pluginsDirs;
}

void PluginRegistry::discoverInstalledModules() {
    std::unique_lock lock(m_mutex);

    PackageManagerLib& pm = packageManagerInstance();
    if (!m_pluginsDirs.empty()) {
        pm.setEmbeddedModulesDirectory(m_pluginsDirs.front());
        for (std::size_t i = 1; i < m_pluginsDirs.size(); ++i) {
            pm.addEmbeddedModulesDirectory(m_pluginsDirs[i]);
        }
    }

    std::string jsonStr = pm.getInstalledModules();
    nlohmann::json modules = nlohmann::json::parse(jsonStr, nullptr, false);
    if (!modules.is_array()) return;

    // Collect names seen in this scan. Used after the upsert loop to prune
    // entries for plugins whose files disappeared (typical path: the user
    // uninstalls a module — its directory is removed, but without pruning
    // the stale PluginInfo would stay in m_plugins forever and
    // knownPluginNames()/`logos_core_get_known_plugins` would keep returning
    // it, so the UI would never see the uninstall land.
    std::unordered_set<std::string> scannedNames;

    for (const auto& mod : modules) {
        std::string name = mod.value("name", "");
        std::string mainFilePath = mod.value("mainFilePath", "");

        if (name.empty() || mainFilePath.empty())
            continue;

        std::string pluginName = processPluginInternal(mainFilePath);
        if (pluginName.empty()) {
            // Skip entries with no extractable metadata — a bare
            // `scannedNames.insert` would record an empty string and the
            // prune loop would mis-identify still-present plugins as gone.
            spdlog::warn("Failed to process plugin: {}", mainFilePath);
            continue;
        }
        scannedNames.insert(pluginName);
    }

    // Prune entries that aren't on disk anymore. Preserve currently-loaded
    // plugins even if their backing files are gone — the module is still
    // running, and the metadata is still needed by unloadPlugin / cascade
    // teardown until it exits. The next discovery after that unload will
    // evict the entry.
    std::vector<std::string> toRemove;
    for (const auto& [name, info] : m_plugins) {
        if (scannedNames.count(name) == 0 && !info.loaded)
            toRemove.push_back(name);
    }
    for (const std::string& name : toRemove) {
        m_plugins.erase(name);
    }

    // Graph has its final shape (upserts + prunes applied). Re-derive
    // dependents so cascade / PluginManager::getDependents can read them
    // directly from PluginInfo without re-querying PackageManagerLib.
    recomputeDependentsLocked();
}

std::string PluginRegistry::processPlugin(const std::string& pluginPath) {
    std::unique_lock lock(m_mutex);
    std::string name = processPluginInternal(pluginPath);
    // A single plugin changed, but its new dependency list can invert edges
    // elsewhere in the graph (e.g. an upgrade that drops a dep). Full
    // rebuild is simpler and still O(N * avg_deps) — cheap at module scale.
    recomputeDependentsLocked();
    return name;
}

std::string PluginRegistry::processPluginInternal(const std::string& pluginPath) {
    std::string name = ModuleLib::LogosModule::getModuleName(pluginPath);
    if (name.empty()) {
        spdlog::warn("No valid metadata for plugin: {}", pluginPath);
        return {};
    }

    // Update plugin info in place so re-discovery preserves the loaded flag
    // (and any other state that lives on PluginInfo).
    PluginInfo& info = m_plugins[name];
    info.path = pluginPath;
    info.dependencies.clear();
    for (const auto& d : ModuleLib::LogosModule::getModuleDependencies(pluginPath)) {
        info.dependencies.push_back(d);
    }

    return name;
}

bool PluginRegistry::isKnown(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    return m_plugins.count(name) > 0;
}

std::string PluginRegistry::pluginPath(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_plugins.find(name);
    return it != m_plugins.end() ? it->second.path : std::string{};
}

std::vector<std::string> PluginRegistry::pluginDependencies(const std::string& name,
                                                            bool recursive) const {
    std::shared_lock lock(m_mutex);
    return pluginDependenciesLocked(name, recursive);
}

std::vector<std::string> PluginRegistry::pluginDependenciesLocked(const std::string& name,
                                                                  bool recursive) const {
    auto it = m_plugins.find(name);
    if (it == m_plugins.end())
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
        auto depIt = m_plugins.find(current);
        if (depIt == m_plugins.end()) continue;
        for (const std::string& d : depIt->second.dependencies) {
            if (seen.count(d) == 0) queue.push_back(d);
        }
    }
    return out;
}

std::vector<std::string> PluginRegistry::pluginDependents(const std::string& name,
                                                          bool recursive) const {
    std::shared_lock lock(m_mutex);
    return pluginDependentsLocked(name, recursive);
}

std::vector<std::string> PluginRegistry::pluginDependentsLocked(const std::string& name,
                                                                bool recursive) const {
    auto it = m_plugins.find(name);
    if (it == m_plugins.end())
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
        auto depIt = m_plugins.find(current);
        if (depIt == m_plugins.end()) continue;
        for (const std::string& d : depIt->second.dependents) {
            if (seen.count(d) == 0) queue.push_back(d);
        }
    }
    return out;
}

void PluginRegistry::recomputeDependentsLocked() {
    // Wipe the reverse edges in place — we don't want to reallocate each
    // PluginInfo, so clear() keeps any existing vector capacity.
    for (auto& [k, v] : m_plugins)
        v.dependents.clear();

    // Invert every forward edge. An entry whose dependency points at an
    // unknown plugin is silently skipped — we can't register a reverse
    // edge against something we don't track, and logging per-edge here
    // would flood the log during every discovery pass.
    for (const auto& [depender, info] : m_plugins) {
        for (const std::string& dep : info.dependencies) {
            auto depIt = m_plugins.find(dep);
            if (depIt == m_plugins.end()) continue;
            auto& deps = depIt->second.dependents;
            if (std::find(deps.begin(), deps.end(), depender) == deps.end())
                deps.push_back(depender);
        }
    }
}

std::vector<std::string> PluginRegistry::knownPluginNames() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::string> keys;
    keys.reserve(m_plugins.size());
    for (const auto& [k, v] : m_plugins)
        keys.push_back(k);
    return keys;
}

void PluginRegistry::registerPlugin(const std::string& name, const std::string& path,
                                    const std::vector<std::string>& dependencies) {
    std::unique_lock lock(m_mutex);
    PluginInfo& info = m_plugins[name];
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

void PluginRegistry::registerDependencies(const std::string& name, const std::vector<std::string>& dependencies) {
    std::unique_lock lock(m_mutex);
    m_plugins[name].dependencies = dependencies;
    // Same reasoning as registerPlugin: this is a direct graph mutator used
    // by tests. Keep the dependents-consistent-with-dependencies invariant
    // holding across every path that edits forward edges.
    recomputeDependentsLocked();
}

bool PluginRegistry::isLoaded(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_plugins.find(name);
    return it != m_plugins.end() && it->second.loaded;
}

void PluginRegistry::markLoaded(const std::string& name) {
    std::unique_lock lock(m_mutex);
    m_plugins[name].loaded = true;
}

void PluginRegistry::markUnloaded(const std::string& name) {
    std::unique_lock lock(m_mutex);
    auto it = m_plugins.find(name);
    if (it != m_plugins.end())
        it->second.loaded = false;
}

std::vector<std::string> PluginRegistry::loadedPluginNames() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::string> result;
    for (const auto& [k, v] : m_plugins) {
        if (v.loaded)
            result.push_back(k);
    }
    return result;
}

void PluginRegistry::clearLoaded() {
    std::unique_lock lock(m_mutex);
    for (auto& [k, v] : m_plugins)
        v.loaded = false;
}

void PluginRegistry::clear() {
    std::unique_lock lock(m_mutex);
    m_pluginsDirs.clear();
    m_plugins.clear();
}
