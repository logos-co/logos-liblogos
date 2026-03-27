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
            pm.addEmbeddedModulesDirectory(m_pluginsDirs[i].toStdString());
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

    PluginInfo info;
    info.path = pluginPath;
    for (const auto& d : ModuleLib::LogosModule::getModuleDependencies(pluginPath.toStdString())) {
        info.dependencies.append(QString::fromStdString(d));
    }
    m_plugins.insert(qName, info);

    return qName;
}

bool PluginRegistry::isKnown(const QString& name) const {
    return m_plugins.contains(name);
}

QString PluginRegistry::pluginPath(const QString& name) const {
    return m_plugins.value(name).path;
}

QStringList PluginRegistry::pluginDependencies(const QString& name) const {
    return m_plugins.value(name).dependencies;
}

QStringList PluginRegistry::knownPluginNames() const {
    return m_plugins.keys();
}

void PluginRegistry::registerPlugin(const QString& name, const QString& path,
                                    const QStringList& dependencies) {
    PluginInfo& info = m_plugins[name];
    info.path = path;
    if (!dependencies.isEmpty())
        info.dependencies = dependencies;
}

void PluginRegistry::registerDependencies(const QString& name, const QStringList& dependencies) {
    m_plugins[name].dependencies = dependencies;
}

bool PluginRegistry::isLoaded(const QString& name) const {
    return m_plugins.value(name).loaded;
}

void PluginRegistry::markLoaded(const QString& name) {
    m_plugins[name].loaded = true;
}

void PluginRegistry::markUnloaded(const QString& name) {
    if (m_plugins.contains(name))
        m_plugins[name].loaded = false;
}

QStringList PluginRegistry::loadedPluginNames() const {
    QStringList result;
    for (auto it = m_plugins.constBegin(); it != m_plugins.constEnd(); ++it) {
        if (it.value().loaded)
            result.append(it.key());
    }
    return result;
}

void PluginRegistry::clearLoaded() {
    for (auto it = m_plugins.begin(); it != m_plugins.end(); ++it)
        it.value().loaded = false;
}

void PluginRegistry::clear() {
    m_pluginsDirs.clear();
    m_plugins.clear();
}
