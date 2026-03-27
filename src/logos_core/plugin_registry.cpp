#include "plugin_registry.h"
#include <QDebug>
#include <QDir>
#include <cassert>
#include <nlohmann/json.hpp>
#include <module_lib/module_lib.h>
#include <package_manager_lib.h>

static PackageManagerLib& packageManagerInstance() {
    static PackageManagerLib instance;
    return instance;
}

void PluginRegistry::setPluginsDir(const QString& dir) {
    m_pluginsDirs.clear();
    m_pluginsDirs.append(dir);
}

void PluginRegistry::addPluginsDir(const QString& dir) {
    if (m_pluginsDirs.contains(dir)) return;
    m_pluginsDirs.append(dir);
}

QStringList PluginRegistry::pluginsDirs() const {
    return m_pluginsDirs;
}

void PluginRegistry::discoverInstalledModules() {
    PackageManagerLib& pm = packageManagerInstance();
    if (!m_pluginsDirs.isEmpty()) {
        pm.setEmbeddedModulesDirectory(m_pluginsDirs.first().toStdString());
        for (int i = 1; i < m_pluginsDirs.size(); ++i) {
            pm.setUserModulesDirectory(m_pluginsDirs[i].toStdString());
        }
    }

    std::string jsonStr = pm.getInstalledModules();
    nlohmann::json modules = nlohmann::json::parse(jsonStr, nullptr, false);
    if (!modules.is_array()) return;

    for (const auto& mod : modules) {
        QString name = QString::fromStdString(mod.value("name", ""));
        QString mainFilePath = QString::fromStdString(mod.value("mainFilePath", ""));

        if (name.isEmpty() || mainFilePath.isEmpty())
            continue;

        QString pluginName = processPlugin(mainFilePath);
        if (pluginName.isEmpty()) {
            qWarning() << "Failed to process plugin:" << mainFilePath;
        }
    }
}

QString PluginRegistry::processPlugin(const QString& pluginPath) {
    std::string name = ModuleLib::LogosModule::getModuleName(pluginPath.toStdString());
    if (name.empty()) {
        qWarning() << "No valid metadata for plugin:" << pluginPath;
        return QString();
    }

    QString qName = QString::fromStdString(name);
    m_knownPlugins.insert(qName, pluginPath);

    auto deps = ModuleLib::LogosModule::getModuleDependencies(pluginPath.toStdString());
    QStringList qDeps;
    for (const auto& d : deps) {
        qDeps.append(QString::fromStdString(d));
    }
    m_pluginDependencies.insert(qName, qDeps);

    return qName;
}

bool PluginRegistry::isKnown(const QString& name) const {
    return m_knownPlugins.contains(name);
}

QString PluginRegistry::pluginPath(const QString& name) const {
    return m_knownPlugins.value(name);
}

QStringList PluginRegistry::pluginDependencies(const QString& name) const {
    return m_pluginDependencies.value(name);
}

QStringList PluginRegistry::knownPluginNames() const {
    return m_knownPlugins.keys();
}

void PluginRegistry::registerPlugin(const QString& name, const QString& path) {
    m_knownPlugins.insert(name, path);
}

void PluginRegistry::registerDependencies(const QString& name, const QStringList& dependencies) {
    m_pluginDependencies.insert(name, dependencies);
}

bool PluginRegistry::isLoaded(const QString& name) const {
    return m_loadedPlugins.contains(name);
}

void PluginRegistry::markLoaded(const QString& name) {
    m_loadedPlugins.append(name);
}

void PluginRegistry::markUnloaded(const QString& name) {
    m_loadedPlugins.removeAll(name);
}

QStringList PluginRegistry::loadedPluginNames() const {
    return m_loadedPlugins;
}

void PluginRegistry::clearLoaded() {
    m_loadedPlugins.clear();
}

void PluginRegistry::clear() {
    m_pluginsDirs.clear();
    m_knownPlugins.clear();
    m_pluginDependencies.clear();
    m_loadedPlugins.clear();
}
