#include "core_manager.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QMetaMethod>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileInfo>
#include <QFile>
#include "../logos_core.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include <module_lib/module_lib.h>

using namespace ModuleLib;

CoreManagerPlugin::CoreManagerPlugin() {
    qDebug() << "CoreManager plugin created";
}

CoreManagerPlugin::~CoreManagerPlugin() {
    cleanup();
}

void CoreManagerPlugin::initialize(int argc, char* argv[]) {
    qDebug() << "Initializing CoreManager plugin";
    // We don't call logos_core_init here because it creates another QApplication
    // instance and we're already creating one in main.cpp
}

void CoreManagerPlugin::setPluginsDirectory(const QString& directory) {
    m_pluginsDirectory = directory;
    qDebug() << "Setting plugins directory to:" << directory;
    logos_core_set_plugins_dir(directory.toUtf8().constData());
}

void CoreManagerPlugin::start() {
    qDebug() << "Starting CoreManager plugin";
    logos_core_start();
}

void CoreManagerPlugin::cleanup() {
    qDebug() << "Cleaning up CoreManager plugin";
    logos_core_cleanup();
}

void CoreManagerPlugin::helloWorld() {
    qDebug() << "Hello from CoreManager plugin!";
}

QStringList CoreManagerPlugin::getLoadedPlugins() {
    qDebug() << "\n\n----------> Getting loaded plugins\n\n";
    QStringList pluginList;
    char** plugins = logos_core_get_loaded_plugins();
    
    if (!plugins) {
        return pluginList;
    }
    
    for (char** p = plugins; *p != nullptr; ++p) {
        pluginList << QString::fromUtf8(*p);
        delete[] *p;
    }
    delete[] plugins;
    
    return pluginList;
}

QJsonArray CoreManagerPlugin::getKnownPlugins() {
    qDebug() << "CoreManager: Getting known plugins with status";
    QJsonArray pluginsArray;
    
    // Get all known plugins
    char** plugins = logos_core_get_known_plugins();
    if (!plugins) {
        return pluginsArray;
    }
    
    // Get all loaded plugins for status checking
    QStringList loadedPlugins = getLoadedPlugins();
    
    // Populate the JSON array with plugin information
    for (char** p = plugins; *p != nullptr; ++p) {
        QString pluginName = QString::fromUtf8(*p);
        
        // Create a JSON object for each plugin
        QJsonObject pluginObj;
        pluginObj["name"] = pluginName;
        pluginObj["loaded"] = loadedPlugins.contains(pluginName);
        
        // Add to array
        pluginsArray.append(pluginObj);
        
        // Free memory
        delete[] *p;
    }
    delete[] plugins;
    
    return pluginsArray;
}

bool CoreManagerPlugin::loadPlugin(const QString& pluginName) {
    qDebug() << "CoreManager: Loading plugin with dependencies:" << pluginName;
    int result = logos_core_load_plugin_with_dependencies(pluginName.toUtf8().constData());
    return result == 1;
}

bool CoreManagerPlugin::unloadPlugin(const QString& pluginName) {
    qDebug() << "CoreManager: Unloading plugin:" << pluginName;
    int result = logos_core_unload_plugin(pluginName.toUtf8().constData());
    return result == 1;
}

QString CoreManagerPlugin::processPlugin(const QString& filePath) {
    qDebug() << "CoreManager: Processing plugin file:" << filePath;
    char* result = logos_core_process_plugin(filePath.toUtf8().constData());

    if (!result) {
        qWarning() << "Failed to process plugin file:" << filePath;
        return QString();
    }

    // Convert to QString and free the C string
    QString pluginName = QString::fromUtf8(result);
    delete[] result;

    return pluginName;
}

// TODO: unclear if in use but it needs to be updated
// TODO: this should NOT be in liblogos in any case and should be moved to its own module
QJsonArray CoreManagerPlugin::getPluginMethods(const QString& pluginName) {
    auto m_logosAPI = new LogosAPI("core_manager", this);

    // Get the plugin using LogosAPI instead of PluginRegistry
    QObject* plugin = nullptr;
    if (m_logosAPI && m_logosAPI->getClient(pluginName)->isConnected()) {
        plugin = m_logosAPI->getClient(pluginName)->requestObject(pluginName);
    }

    if (!plugin) {
        qWarning() << "Plugin not found:" << pluginName;
        return QJsonArray();
    }

    // Use module_lib for runtime introspection
    QJsonArray methodsArray = LogosModule::getMethodsAsJson(plugin, true);

    // Clean up the replica object when done
    delete plugin;

    return methodsArray;
}

void CoreManagerPlugin::initLogos(LogosAPI* logosAPIInstance) {
    qDebug() << "CoreManager: initLogos called with LogosAPI instance";
    logosAPI = logosAPIInstance;
    qDebug() << "CoreManager: LogosAPI instance stored successfully";
}
