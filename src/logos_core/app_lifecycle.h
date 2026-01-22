#ifndef APP_LIFECYCLE_H
#define APP_LIFECYCLE_H

#include <QStringList>

namespace AppLifecycle {
    // Initialize the logos core application
    void init(int argc, char* argv[]);
    
    // Set the SDK communication mode (0=Remote, 1=Local)
    void setMode(int mode);
    
    // Set the custom plugins directory (replaces existing)
    void setPluginsDir(const char* plugins_dir);
    
    // Add an additional plugins directory to scan
    void addPluginsDir(const char* plugins_dir);
    
    // Start the logos core functionality (discover plugins, init core manager)
    void start();
    
    // Run the event loop
    int exec();
    
    // Clean up all resources (plugins, processes, registry host, app)
    void cleanup();
    
    // Process Qt events without blocking
    void processEvents();
    
    // Query methods
    QStringList getPluginsDirs();
    bool isInitialized();           // returns true if g_app is set
    bool isAppOwnedByUs();          // returns true if we created the app
    bool isRegistryHostInitialized(); // returns true if registry host exists
}

#endif // APP_LIFECYCLE_H
