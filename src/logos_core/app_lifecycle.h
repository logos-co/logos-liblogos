#ifndef APP_LIFECYCLE_H
#define APP_LIFECYCLE_H

namespace AppLifecycle {
    // Initialize the logos core application
    void init(int argc, char* argv[]);

    // Set the custom plugins directory (replaces existing)
    void setPluginsDir(const char* plugins_dir);
    
    // Add an additional plugins directory to scan
    void addPluginsDir(const char* plugins_dir);
    
    // Start the logos core functionality (discover plugins, init capability module)
    void start();
    
    // Run the event loop
    int exec();
    
    // Clean up all resources (plugins, processes, registry host, app)
    void cleanup();
    
    // Process Qt events without blocking
    void processEvents();
}

#endif // APP_LIFECYCLE_H
