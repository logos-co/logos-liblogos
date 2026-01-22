#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include <QString>
#include <QStringList>
#include <QHash>

namespace PluginManager {
    // Process a plugin file and extract its metadata
    // Returns the plugin name if successful, empty string otherwise
    QString processPlugin(const QString& pluginPath);
    
    // Load a plugin by name (handles both Local and Remote modes)
    // Returns true if successful, false otherwise
    bool loadPlugin(const QString& pluginName);
    
    // Load and process a plugin in one step
    void loadAndProcessPlugin(const QString& pluginPath);
    
    // Find all plugins in a directory
    // Returns a list of full paths to plugin files
    QStringList findPlugins(const QString& pluginsDir);
    
    // Initialize the core manager
    // Returns true if successful, false otherwise
    bool initializeCoreManager();
    
    // Initialize the capability module if available
    // Returns true if successful, false otherwise
    bool initializeCapabilityModule();
    
    // Load all statically linked plugins (for iOS/mobile)
    // Returns the number of plugins successfully loaded
    int loadStaticPlugins();
    
    // Register a plugin instance directly (for iOS/mobile)
    // Returns true if successful, false otherwise
    bool registerPluginInstance(const QString& pluginName, void* pluginInstance);
    
    // Register a statically-linked plugin by name (Local mode only)
    // Returns true if successful, false otherwise
    bool registerPluginByName(const QString& pluginName);
    
    // Unload a plugin by name (Remote mode only)
    // Returns true if successful, false otherwise
    bool unloadPlugin(const QString& pluginName);
    
    // Get loaded plugins as a C string array (for C API)
    // Returns a null-terminated array of plugin names that must be freed by the caller
    char** getLoadedPluginsCStr();
    
    // Get known plugins as a C string array (for C API)
    // Returns a null-terminated array of plugin names that must be freed by the caller
    char** getKnownPluginsCStr();
    
    // Query methods (Qt types for C++ usage)
    QStringList getLoadedPlugins();
    QHash<QString, QString> getKnownPlugins();
    bool isPluginLoaded(const QString& name);
    bool isPluginKnown(const QString& name);
}

#endif // PLUGIN_MANAGER_H
