#ifndef LOGOS_CORE_H
#define LOGOS_CORE_H

// Define export macro for the library
#if defined(LOGOS_CORE_LIBRARY)
#  define LOGOS_CORE_EXPORT __attribute__((visibility("default")))
#else
#  define LOGOS_CORE_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the logos core library
LOGOS_CORE_EXPORT void logos_core_init(int argc, char *argv[]);

// Set the SDK communication mode
// mode: 0 = Remote (default, uses separate processes via logos_host)
//       1 = Local (in-process, for mobile apps)
// Must be called before logos_core_start()
#define LOGOS_MODE_REMOTE 0
#define LOGOS_MODE_LOCAL 1
LOGOS_CORE_EXPORT void logos_core_set_mode(int mode);

// Set a custom plugins directory
LOGOS_CORE_EXPORT void logos_core_set_plugins_dir(const char* plugins_dir);

// Add an additional plugins directory to scan (allows multiple directories)
LOGOS_CORE_EXPORT void logos_core_add_plugins_dir(const char* plugins_dir);

// Start the logos core functionality
LOGOS_CORE_EXPORT void logos_core_start();

// Run the event loop
LOGOS_CORE_EXPORT int logos_core_exec();

// Clean up resources
LOGOS_CORE_EXPORT void logos_core_cleanup();

// Get the list of loaded plugins
// Returns a null-terminated array of plugin names that must be freed by the caller
LOGOS_CORE_EXPORT char** logos_core_get_loaded_plugins();

// Get the list of known plugins
// Returns a null-terminated array of plugin names that must be freed by the caller
LOGOS_CORE_EXPORT char** logos_core_get_known_plugins();

// Load a specific plugin by name
// Returns 1 if successful, 0 if failed
LOGOS_CORE_EXPORT int logos_core_load_plugin(const char* plugin_name);

// Unload a specific plugin by name
// Returns 1 if successful, 0 if failed
LOGOS_CORE_EXPORT int logos_core_unload_plugin(const char* plugin_name);

// Process a plugin file and add it to known plugins
// Returns the plugin name if successful, NULL if failed
LOGOS_CORE_EXPORT char* logos_core_process_plugin(const char* plugin_path);

// Load all statically linked plugins (for iOS/mobile where dynamic loading is unavailable)
// Uses Qt's static plugin mechanism via Q_IMPORT_PLUGIN
// Must be called after logos_core_start() in Local mode
// Returns the number of plugins successfully loaded
LOGOS_CORE_EXPORT int logos_core_load_static_plugins();

// Register a plugin instance directly (for iOS/mobile where the app creates plugin instances)
// plugin_name: The name of the plugin (e.g., "package_manager")
// plugin_instance: Pointer to the plugin QObject instance (must implement PluginInterface)
// Must be called after logos_core_start() in Local mode
// Returns 1 if successful, 0 if failed
LOGOS_CORE_EXPORT int logos_core_register_plugin_instance(const char* plugin_name, void* plugin_instance);

// Register a statically-linked plugin by name (Local mode only)
// Must be called after logos_core_start() in Local mode
// Returns 1 if successful, 0 if failed
LOGOS_CORE_EXPORT int logos_core_register_plugin_by_name(const char* plugin_name);

// Get a token by key from the core token manager
// Returns the token value if found, NULL if not found
// The returned string must be freed by the caller
LOGOS_CORE_EXPORT char* logos_core_get_token(const char* key);

// Get module statistics (CPU and memory usage) for all loaded modules
// Returns a JSON string containing array of module stats, NULL on error
// The returned string must be freed by the caller
// Excludes core_manager as it runs in-process
LOGOS_CORE_EXPORT char* logos_core_get_module_stats();

// === Async Callback API ===

// Define the callback function type for async operations
typedef void (*AsyncCallback)(int result, const char* message, void* user_data);

// Simple async operation example that uses a callback
LOGOS_CORE_EXPORT void logos_core_async_operation(const char* data, AsyncCallback callback, void* user_data);

// Async plugin loading with callback
LOGOS_CORE_EXPORT void logos_core_load_plugin_async(const char* plugin_name, AsyncCallback callback, void* user_data);

// Proxy method to call plugin methods remotely with async callback
// params_json: JSON string containing array of {name, value, type} objects
LOGOS_CORE_EXPORT void logos_core_call_plugin_method_async(
    const char* plugin_name, 
    const char* method_name, 
    const char* params_json, 
    AsyncCallback callback, 
    void* user_data
);

// Register an event listener for a specific event from a specific plugin
// The callback will be triggered whenever the specified event is emitted by the plugin
LOGOS_CORE_EXPORT void logos_core_register_event_listener(
    const char* plugin_name,
    const char* event_name, 
    AsyncCallback callback,
    void* user_data
);

// Process Qt events without blocking (for integration with other event loops)
LOGOS_CORE_EXPORT void logos_core_process_events();

#ifdef __cplusplus
}
#endif

#endif // LOGOS_CORE_H 