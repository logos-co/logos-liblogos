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
#else
// `bool` in C requires <stdbool.h>. C++ has it built-in as a keyword.
#include <stdbool.h>
#endif

// Initialize the logos core library
LOGOS_CORE_EXPORT void logos_core_init(int argc, char *argv[]);

// Set a custom modules directory
LOGOS_CORE_EXPORT void logos_core_set_modules_dir(const char* modules_dir);

// Add an additional modules directory to scan (allows multiple directories)
LOGOS_CORE_EXPORT void logos_core_add_modules_dir(const char* modules_dir);

// Start the logos core functionality
LOGOS_CORE_EXPORT void logos_core_start();

// Run the event loop
LOGOS_CORE_EXPORT int logos_core_exec();

// Clean up resources
LOGOS_CORE_EXPORT void logos_core_cleanup();

// Get the list of loaded modules
// Returns a null-terminated array of module names that must be freed by the caller
LOGOS_CORE_EXPORT char** logos_core_get_loaded_modules();

// Get the list of known modules
// Returns a null-terminated array of module names that must be freed by the caller
LOGOS_CORE_EXPORT char** logos_core_get_known_modules();

// Load a specific module by name
// Returns 1 if successful, 0 if failed
LOGOS_CORE_EXPORT int logos_core_load_module(const char* module_name);

// Load a module and all its dependencies automatically
// Resolves the dependency tree and loads modules in correct order
// Returns 1 if all modules loaded successfully, 0 if any failed
LOGOS_CORE_EXPORT int logos_core_load_module_with_dependencies(const char* module_name);

// Unload a specific module by name
// Returns 1 if successful, 0 if failed
LOGOS_CORE_EXPORT int logos_core_unload_module(const char* module_name);

// Unload a module together with every loaded module that (transitively)
// depends on it. Dependents come down first (leaves-first) so no process is
// briefly pointing at a terminated parent. Returns 1 only if every step
// succeeded.
LOGOS_CORE_EXPORT int logos_core_unload_module_with_dependents(const char* module_name);

// Return the modules that `module_name` depends on (forward edges).
// If `recursive` is true, returns the full transitive dependency closure
// reached by a breadth-first walk; the target itself is not included.
// Unknown names yield a zero-length array (just a trailing NULL).
// Returns a null-terminated array of module names that must be freed by the caller.
LOGOS_CORE_EXPORT char** logos_core_get_module_dependencies(const char* module_name, bool recursive);

// Return the modules that depend on `module_name` (reverse edges).
// If `recursive` is true, returns the full transitive dependent closure.
// Unknown names yield a zero-length array (just a trailing NULL).
// Returns a null-terminated array of module names that must be freed by the caller.
LOGOS_CORE_EXPORT char** logos_core_get_module_dependents(const char* module_name, bool recursive);

// Process a module file and add it to known modules
// Returns the module name if successful, NULL if failed
LOGOS_CORE_EXPORT char* logos_core_process_module(const char* module_path);

// Get a token by key from the core token manager
// Returns the token value if found, NULL if not found
// The returned string must be freed by the caller
LOGOS_CORE_EXPORT char* logos_core_get_token(const char* key);

// Get module statistics (CPU and memory usage) for all loaded modules
// Returns a JSON string containing array of module stats, NULL on error
// The returned string must be freed by the caller
LOGOS_CORE_EXPORT char* logos_core_get_module_stats();

// Set the base directory for module instance persistence.
// Each module gets a subdirectory: {path}/{module_name}/{instance_id}/
// Must be called before logos_core_start().
LOGOS_CORE_EXPORT void logos_core_set_persistence_base_path(const char* path);

// Re-scan all module directories and update known modules.
// Call after installing new modules so they become discoverable.
LOGOS_CORE_EXPORT void logos_core_refresh_modules();


// Process Qt events without blocking (for integration with other event loops)
LOGOS_CORE_EXPORT void logos_core_process_events();

// =========================================================================
// DEPRECATED — old "plugin" names kept for backward compatibility.
// Each forwards to the corresponding logos_core_*_module* function above.
// =========================================================================

LOGOS_CORE_EXPORT void logos_core_set_plugins_dir(const char* plugins_dir);
LOGOS_CORE_EXPORT void logos_core_add_plugins_dir(const char* plugins_dir);
LOGOS_CORE_EXPORT char** logos_core_get_loaded_plugins();
LOGOS_CORE_EXPORT char** logos_core_get_known_plugins();
LOGOS_CORE_EXPORT int logos_core_load_plugin(const char* plugin_name);
LOGOS_CORE_EXPORT int logos_core_load_plugin_with_dependencies(const char* plugin_name);
LOGOS_CORE_EXPORT int logos_core_unload_plugin(const char* plugin_name);
LOGOS_CORE_EXPORT int logos_core_unload_plugin_with_dependents(const char* plugin_name);
LOGOS_CORE_EXPORT char* logos_core_process_plugin(const char* plugin_path);
LOGOS_CORE_EXPORT void logos_core_refresh_plugins();

#ifdef __cplusplus
}
#endif

#endif // LOGOS_CORE_H
