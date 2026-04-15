#include "plugin_registry.h"
#include <spdlog/spdlog.h>
#include <cassert>
#include <mutex>
#include <shared_mutex>
#include <algorithm>
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

    for (const auto& mod : modules) {
        std::string name = mod.value("name", "");
        std::string mainFilePath = mod.value("mainFilePath", "");

        if (name.empty() || mainFilePath.empty())
            continue;

        std::string pluginName = processPluginInternal(mainFilePath);
        if (pluginName.empty()) {
            spdlog::warn("Failed to process plugin: {}", mainFilePath);
        }
    }
}

std::string PluginRegistry::processPlugin(const std::string& pluginPath) {
    std::unique_lock lock(m_mutex);
    return processPluginInternal(pluginPath);
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

std::vector<std::string> PluginRegistry::pluginDependencies(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto it = m_plugins.find(name);
    return it != m_plugins.end() ? it->second.dependencies : std::vector<std::string>{};
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
    if (!dependencies.empty())
        info.dependencies = dependencies;
}

void PluginRegistry::registerDependencies(const std::string& name, const std::vector<std::string>& dependencies) {
    std::unique_lock lock(m_mutex);
    m_plugins[name].dependencies = dependencies;
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
