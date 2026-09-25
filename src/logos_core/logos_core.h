#ifndef LOGOS_CORE_H
#define LOGOS_CORE_H

// Define export macro for the library.
//
// Windows needs __declspec, not visibility attributes: mingw-gcc accepts
// __attribute__((visibility)) and silently ignores it on PE, so this header
// used to export nothing at all and the build compensated with
// -Wl,--export-all-symbols. That worked, but it exported the ENTIRE image --
// 13,252 symbols, of which only 18 are this C API -- including every internal
// C++ symbol such as LogosAPI's. Any consumer that links liblogos_core AND the
// qt-sdk static library then gets the same definition twice and the link fails
// with "multiple definition of `LogosAPI::LogosAPI'".
//
// Annotating the C API explicitly fixes that at the root, and does so twice
// over: GNU ld disables PE auto-export image-wide as soon as ANY symbol is
// dllexported, so the internal C++ surface stops leaking as a side effect.
#if defined(_WIN32)
#  if defined(LOGOS_CORE_LIBRARY)
#    define LOGOS_CORE_EXPORT __declspec(dllexport)
#  else
#    define LOGOS_CORE_EXPORT __declspec(dllimport)
#  endif
#elif defined(LOGOS_CORE_LIBRARY)
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

// The directories the embedder ships its own modules in: protected input, taken
// before logos_core_start() only. They are scanned too. Once set, a reserved
// module name (core_service, capability_module, modules_state, package_manager,
// package_downloader, logos_*, the first-party shell names; compared without
// case) resolves only from them, and only their modules receive host services
// or run in-process. `dirs` is NULL-terminated.
// Returns 0, or -1 after logos_core_start() or for a NULL `dirs`.
LOGOS_CORE_EXPORT int logos_core_set_bundled_modules_dirs(const char* const* dirs);

// Where modules run: protected input, before logos_core_start() only.
//     {"default": "subprocess" | "inproc",
//      "modules": {"<name>": "subprocess" | "inproc"},
//      "single_process": false}
// A module runs in-process only when it is bundled, its build stamped it
// in-process eligible, and the policy (or, when silent, the runtime's own
// table) places it here; otherwise it runs in its own host process.
// capability_module, the token authority, runs in-process or not at all. With
// single_process, a module that cannot run in-process is not loaded at all.
// NULL or "" restores the default. Returns 0, or -1 after start or for a
// malformed policy.
LOGOS_CORE_EXPORT int logos_core_set_placement_policy(const char* policy_json);

// package_manager's settings, applied by the runtime as it loads (its setters
// answer only the runtime): protected input, before logos_core_start() only.
//     {"embedded_modules_dirs": [...], "user_modules_dir": "...",
//      "embedded_ui_plugins_dirs": [...], "user_ui_plugins_dir": "...",
//      "keyring_dir": "...", "signature_policy": "none" | "warn" | "require"}
// Every key is optional. A signature policy package_manager does not take fails
// its load. Returns 0, or -1 after start or for a malformed document.
LOGOS_CORE_EXPORT int logos_core_set_package_config(const char* config_json);

// ── core_service, the runtime's control surface ──────────────────────────────
// It is published at logos_core_start() as a module of its own, on inproc and
// the local socket. Each method answers only the callers its scope admits:
// any admitted module reads; the shell and operators load and unload; only the
// shell admits presentation consumers; only operators forward calls (never to
// the runtime's own modules). These setters are protected input, taken before
// logos_core_start() only, and return 0 or -1.

// Further transports for it (tcp, tcp_ssl), as a JSON array.
LOGOS_CORE_EXPORT int logos_core_set_core_service_transports(const char* transports_json);

// Where core_service.shutdown goes; without one it is refused.
typedef void (*LogosCoreShutdownHandler)(void* user_data);
LOGOS_CORE_EXPORT int logos_core_set_shutdown_handler(LogosCoreShutdownHandler handler,
                                                      void* user_data);

// Names the operator a presented token belongs to: a malloc'd name, or NULL.
typedef char* (*LogosCoreOperatorResolver)(const char* token, const char* transport,
                                           void* user_data);
LOGOS_CORE_EXPORT int logos_core_set_operator_resolver(LogosCoreOperatorResolver resolver,
                                                       void* user_data);

// Methods of the embedder's own on core_service: `methods_json` lists them as
// core_service.getMethods does, and `extension` answers each (malloc'd JSON, or
// NULL for a method that is not its), authorizing by the caller document itself.
typedef char* (*LogosCoreServiceExtension)(const char* caller_json, const char* method,
                                           const char* args_json, void* user_data);
LOGOS_CORE_EXPORT int logos_core_set_core_service_extension(LogosCoreServiceExtension extension,
                                                            const char* methods_json,
                                                            void* user_data);

// ── the shell binding ─────────────────────────────────────────────────────────
// The embedder's own identity ("basecamp", "logoscore", ...), set before start.
// capability_module admits it like any consumer, so the modules it calls see
// {"kind":"module","name":<shell>}. Loading, unloading and every query go
// through core_service over this binding.
LOGOS_CORE_EXPORT int logos_core_set_shell_identity(const char* name);

typedef struct logos_consumer logos_consumer;
typedef struct logos_consumer_subscription logos_consumer_subscription;
typedef void (*logos_consumer_result_cb)(int ok, const char* json, void* user_data);
typedef void (*logos_consumer_event_cb)(const char* event_name, const char* data_json,
                                        void* user_data);

// The binding, once, after start; NULL without a shell identity.
LOGOS_CORE_EXPORT logos_consumer* logos_core_take_shell_binding(void);
LOGOS_CORE_EXPORT const char* logos_consumer_name(const logos_consumer* consumer);
// Its credential, for a co-process that acts as the shell. Free with logos_consumer_string_free.
LOGOS_CORE_EXPORT char* logos_consumer_credential(const logos_consumer* consumer);
// As lp_invoke; strings are freed with logos_consumer_string_free.
LOGOS_CORE_EXPORT int logos_consumer_call(logos_consumer* consumer, const char* target,
                                          const char* method, const char* args_json,
                                          int timeout_ms, char** out_result_json,
                                          char** out_error_json);
LOGOS_CORE_EXPORT int logos_consumer_call_async(logos_consumer* consumer, const char* target,
                                                const char* method, const char* args_json,
                                                int timeout_ms, logos_consumer_result_cb cb,
                                                void* user_data);
LOGOS_CORE_EXPORT logos_consumer_subscription* logos_consumer_subscribe(
    logos_consumer* consumer, const char* target, const char* event_name,
    logos_consumer_event_cb cb, void* user_data);
LOGOS_CORE_EXPORT void logos_consumer_unsubscribe(logos_consumer_subscription* subscription);
LOGOS_CORE_EXPORT void logos_consumer_string_free(char* value);
// Ends the handle's calls and subscriptions; the identity lasts until cleanup.
LOGOS_CORE_EXPORT void logos_consumer_release(logos_consumer* consumer);

// Start the runtime: scan the module directories, load capability_module (the
// token authority), publish core_service, load modules_state, and prepare the
// shell binding. capability_module must be bundled and run in-process: without
// it there is no authority, the runtime logs why and stays inert (no
// core_service, no binding), and nothing will load.
//
// Module lifecycle and queries are core_service methods, called over the shell
// binding: loadModule, unloadModule, reloadModule, refreshModules,
// listModules, getModuleInfo, getModulesInfo, getModuleStats,
// getModuleDependencies, getModuleDependents, getModuleOptionalDependencies
// and getOptionalLoadReport (logos-cpp-sdk ships the contract,
// core_service.lidl). loadModule means "ensure loaded": it answers ok for a
// module already up, and it blocks for the bring-up. Loads of different
// modules run at the same time.
LOGOS_CORE_EXPORT void logos_core_start();

// Clean up resources
LOGOS_CORE_EXPORT void logos_core_cleanup();

// Process a module file and add it to known modules. The embedder's alone: it
// is not a core_service method.
// Returns the module name if successful, NULL if failed; free with delete[].
LOGOS_CORE_EXPORT char* logos_core_process_module(const char* module_path);

// Set the base directory for module instance persistence.
// Each module gets a subdirectory: {path}/{module_name}/{instance_id}/
// Must be called before logos_core_start().
LOGOS_CORE_EXPORT void logos_core_set_persistence_base_path(const char* path);

// Register a per-module transport set for the named module. The loader
// passes this through to the module's child subprocess so its
// LogosAPIProvider binds every transport in the set instead of only
// the global default (LocalSocket).
//
// `transport_set_json` is a JSON array of LogosTransportConfig values
// (see logos-cpp-sdk/cpp/logos_transport_config_json.h for the shape).
// NULL or "" clears any previously-registered entry.
//
// Must be called BEFORE the module is loaded — for capability_module
// this means before logos_core_start(); for user modules, before
// core_service loads them. Modules without an entry continue to
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

#ifdef __cplusplus
}
#endif

#endif // LOGOS_CORE_H
