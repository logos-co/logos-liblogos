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

// Load a plugin and all its dependencies automatically
// Resolves the dependency tree and loads plugins in correct order
// Returns 1 if all plugins loaded successfully, 0 if any failed
LOGOS_CORE_EXPORT int logos_core_load_plugin_with_dependencies(const char* plugin_name);

// Unload a specific plugin by name
// Returns 1 if successful, 0 if failed
LOGOS_CORE_EXPORT int logos_core_unload_plugin(const char* plugin_name);

// Process a plugin file and add it to known plugins
// Returns the plugin name if successful, NULL if failed
LOGOS_CORE_EXPORT char* logos_core_process_plugin(const char* plugin_path);

// Get a token by key from the core token manager
// Returns the token value if found, NULL if not found
// The returned string must be freed by the caller
LOGOS_CORE_EXPORT char* logos_core_get_token(const char* key);

// Get module statistics (CPU and memory usage) for all loaded modules
// Returns a JSON string containing array of module stats, NULL on error
// The returned string must be freed by the caller
LOGOS_CORE_EXPORT char* logos_core_get_module_stats();

// Re-scan all plugin directories and update known plugins.
// Call after installing new modules so they become discoverable.
LOGOS_CORE_EXPORT void logos_core_refresh_plugins();


// Get the type of a loaded plugin ("core", "ui", "grpc", etc.)
// Returns the type string if found, NULL if not found
// The returned string must be freed by the caller
LOGOS_CORE_EXPORT char* logos_core_get_plugin_type(const char* plugin_name);

// Get the gRPC socket path for a loaded gRPC plugin
// Returns the socket path if found, NULL if not found or not a gRPC plugin
// The returned string must be freed by the caller
LOGOS_CORE_EXPORT char* logos_core_get_plugin_socket(const char* plugin_name);

// Process Qt events without blocking (for integration with other event loops)
LOGOS_CORE_EXPORT void logos_core_process_events();

#ifdef __cplusplus
}
#endif

#endif // LOGOS_CORE_H 