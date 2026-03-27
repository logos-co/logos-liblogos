#include "plugin_registry.h"
#include <QDebug>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <cassert>
#include <module_lib/module_lib.h>
#include <package_manager_lib.h>

using namespace ModuleLib;

static PackageManagerLib& packageManagerInstance() {
    static PackageManagerLib instance;
    return instance;
}

void PluginRegistry::setPluginsDir(const QString& dir) {
    m_pluginsDirs.clear();
    m_pluginsDirs.append(dir);
    qInfo() << "Custom plugins directory set to:" << m_pluginsDirs.first();
}

void PluginRegistry::addPluginsDir(const QString& dir) {
    if (m_pluginsDirs.contains(dir)) return;
    m_pluginsDirs.append(dir);
    qDebug() << "Added plugins directory:" << dir;
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
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(jsonStr));
    QJsonArray modules = doc.array();

    for (const QJsonValue& val : modules) {
        QJsonObject mod = val.toObject();
        QString name = mod.value("name").toString();
        QString mainFilePath = mod.value("mainFilePath").toString();

        if (name.isEmpty() || mainFilePath.isEmpty())
            continue;

        QString pluginName = processPlugin(mainFilePath);
        if (pluginName.isEmpty()) {
            qWarning() << "Failed to process plugin (no metadata or invalid):" << mainFilePath;
        } else {
            qDebug() << "Discovered module:" << pluginName << "at" << mainFilePath;
        }
    }

    qDebug() << "Total known plugins after discovery:" << m_knownPlugins.size();
    qDebug() << "Known plugin names:" << m_knownPlugins.keys();
}

QString PluginRegistry::processPlugin(const QString& pluginPath) {
    auto metadataOpt = LogosModule::extractMetadata(pluginPath);
    if (!metadataOpt) {
        qWarning() << "No metadata found for plugin:" << pluginPath;
        return QString();
    }

    const ModuleMetadata& metadata = *metadataOpt;
    if (!metadata.isValid()) {
        qWarning() << "Plugin name not specified in metadata for:" << pluginPath;
        return QString();
    }

    m_knownPlugins.insert(metadata.name, pluginPath);
    m_pluginMetadata.insert(metadata.name, metadata.rawMetadata);

    return metadata.name;
}

bool PluginRegistry::isKnown(const QString& name) const {
    return m_knownPlugins.contains(name);
}

QString PluginRegistry::pluginPath(const QString& name) const {
    return m_knownPlugins.value(name);
}

QJsonObject PluginRegistry::pluginMetadata(const QString& name) const {
    return m_pluginMetadata.value(name);
}

QStringList PluginRegistry::knownPluginNames() const {
    return m_knownPlugins.keys();
}

void PluginRegistry::registerPlugin(const QString& name, const QString& path) {
    m_knownPlugins.insert(name, path);
}

void PluginRegistry::registerMetadata(const QString& name, const QJsonObject& metadata) {
    m_pluginMetadata.insert(name, metadata);
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
    m_pluginMetadata.clear();
    m_loadedPlugins.clear();
}
