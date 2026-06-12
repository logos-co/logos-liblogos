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

// Add a modules directory to scan (allows multiple directories).
// Duplicate paths are silently ignored.
LOGOS_CORE_EXPORT void logos_core_add_modules_dir(const char* modules_dir);

// Start the logos core functionality
LOGOS_CORE_EXPORT void logos_core_start();

// Clean up resources
LOGOS_CORE_EXPORT void logos_core_cleanup();

// Get the list of loaded modules
// Returns a null-terminated array of module names that must be freed by the caller
LOGOS_CORE_EXPORT char** logos_core_get_loaded_modules();

// Get the list of known modules
// Returns a null-terminated array of module names that must be freed by the caller
LOGOS_CORE_EXPORT char** logos_core_get_known_modules();

// Load a specific module by name.
// When with_dependencies is true, resolves the dependency tree and loads
// modules in correct topological order before loading the target.
//
// Semantics: "ensure loaded", not "load fresh". Returns 1 when all
// required modules end up loaded — including the case where the target
// (or, with with_dependencies=true, any of its deps) was already loaded
// before the call. This idempotency is load-bearing for callers that
// use it as a guard ("make sure X is up before I use it").
// Returns 0 only when the module is unknown, dependency resolution
// fails, or an actual load step (not an already-loaded no-op) fails.
// Aborts the process if `module_name` is NULL.
LOGOS_CORE_EXPORT int logos_core_load_module(const char* module_name, bool with_dependencies);

// Unload a specific module by name.
// When with_dependents is true, also unloads every loaded module that
// (transitively) depends on it. Dependents come down first (leaves-first)
// so no process is briefly pointing at a terminated parent.
// Returns 1 if successful, 0 if failed
LOGOS_CORE_EXPORT int logos_core_unload_module(const char* module_name, bool with_dependents);

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

// Register a per-module transport set for the named module. The runtime
// passes this through to the module's child subprocess so its
// LogosAPIProvider binds every transport in the set instead of only
// the global default (LocalSocket).
//
// `transport_set_json` is a JSON array of LogosTransportConfig values
// (see logos-cpp-sdk/cpp/logos_transport_config_json.h for the shape).
// NULL or "" clears any previously-registered entry.
//
// Must be called BEFORE the module is loaded — for capability_module
// this means before logos_core_start(); for user modules, before any
// logos_core_load_module() call. Modules without an entry continue to
// inherit the global default.
LOGOS_CORE_EXPORT void logos_core_set_module_transports(const char* module_name,
                                                         const char* transport_set_json);

// Install the inter-module access policy: which callers may invoke which
// targets. `policy_json` shape:
//
//     {
//       "version": 1,
//       "mode": "enforce",
//       "restrictions": {
//         "package_manager":    { "allowedCallers": ["package_manager_ui"] },
//         "package_downloader": { "allowedCallers": ["package_manager_ui"] }
//       }
//     }
//
// A restricted target rejects callers outside its allowlist; a target
// absent from `restrictions` is unrestricted. Only `mode` == "enforce"
// activates gating (any other value registers nothing). Enforced by
// capability_module, which won't issue a token — hence won't allow the
// call — for a disallowed caller.
//
// Must be called before logos_core_start(). NULL or "" clears the policy.
LOGOS_CORE_EXPORT void logos_core_set_access_policy(const char* policy_json);

// Re-scan all module directories and update known modules.
// Call after installing new modules so they become discoverable.
LOGOS_CORE_EXPORT void logos_core_refresh_modules();

#ifdef __cplusplus
}
#endif

#endif // LOGOS_CORE_H
