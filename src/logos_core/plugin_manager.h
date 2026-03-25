#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include <QString>
#include <QStringList>
#include <QHash>

namespace PluginManager {
    // Plugin directory management
    void setPluginsDir(const char* plugins_dir);
    void addPluginsDir(const char* plugins_dir);

    // Resolve all plugin directories (bundled + user-specified), scan them,
    // and populate the known plugins registry
    void discoverPlugins();

    // Process a plugin file and extract its metadata
    // Returns the plugin name if successful, empty string otherwise
    QString processPlugin(const QString& pluginPath);
    
    // Load a plugin by name (spawns logos_host process)
    // Returns true if successful, false otherwise
    bool loadPlugin(const QString& pluginName);

    // Load and process a plugin in one step
    void loadAndProcessPlugin(const QString& pluginPath);
    
    // Find all plugins in a directory
    // Returns a list of full paths to plugin files
    QStringList findPlugins(const QString& pluginsDir);
    
    // Initialize the capability module if available
    // Returns true if successful, false otherwise
    bool initializeCapabilityModule();
    
    // Unload a plugin by name
    // Returns true if successful, false otherwise
    bool unloadPlugin(const QString& pluginName);

    // Terminate all running plugin processes
    void terminateAll();
    
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
    
    // Resolve dependencies for a list of modules
    // Returns the modules in load order (dependencies first), including all transitive dependencies
    // Handles circular dependency detection and missing dependency warnings
    QStringList resolveDependencies(const QStringList& requestedModules);
    
    // State management
    // Clear all plugin state (for shutdown/reinitialization)
    void clearState();
    
    // Programmatically register a known plugin without file discovery
    // Useful when plugin locations are known ahead of time
    void addKnownPlugin(const QString& name, const QString& path);
}

#endif // PLUGIN_MANAGER_H
