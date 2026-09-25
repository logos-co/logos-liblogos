#include "logos_core.h"
#include "logging/logos_log.h"
#include "module_manager.h"
#include "module_registry.h"
#include "bootstrap_policy.h"
#include "core_service/embedded_core_service.h"
#include "core_service/shell_binding.h"
#include <process_stats/process_stats.h>
#include "logos_protocol.h"
#include <atomic>
#include <chrono>
#include <random>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// === C API Implementation (Thin Wrappers) ===

void logos_core_init(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
}

void logos_core_add_modules_dir(const char* modules_dir) {
    ModuleManager::addModulesDir(modules_dir);
}

namespace {
void ensureInstanceId() {
    if (const char* current = std::getenv("LOGOS_INSTANCE_ID"); current && *current)
        return;
    // Twelve hex digits, as the Qt runtime's LogosInstance::id() made them:
    // the id is in every socket path, and macOS caps those at 104 bytes.
    std::random_device random;
    const std::uint64_t bits = (static_cast<std::uint64_t>(random()) << 32 | random())
        ^ static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    char text[13];
    std::snprintf(text, sizeof text, "%012llx",
                  static_cast<unsigned long long>(bits & 0xffffffffffffULL));
    const std::string value = text;
#ifdef _WIN32
    _putenv_s("LOGOS_INSTANCE_ID", value.c_str());
#else
    ::setenv("LOGOS_INSTANCE_ID", value.c_str(), 1);
#endif
}
}

int logos_core_set_bundled_modules_dirs(const char* const* dirs) {
    if (!dirs || ModuleManager::started()) {
        logos::logger("core").error("logos_core_set_bundled_modules_dirs: {}",
                                    dirs ? "refused after logos_core_start()" : "dirs must not be null");
        return -1;
    }
    std::vector<std::string> list;
    for (const char* const* dir = dirs; *dir; ++dir) list.emplace_back(*dir);
    ModuleManager::setBundledModulesDirs(list);
    return 0;
}

int logos_core_set_placement_policy(const char* policy_json) {
    if (ModuleManager::started()) {
        logos::logger("core").error("logos_core_set_placement_policy: refused after logos_core_start()");
        return -1;
    }
    std::string error;
    if (!ModuleManager::setPlacementPolicy(policy_json ? policy_json : "", error)) {
        logos::logger("core").error("logos_core_set_placement_policy: {}", error);
        return -1;
    }
    return 0;
}

namespace {
// A protected setter answers only before start.
bool beforeStart(const char* setter) {
    if (!ModuleManager::started()) return true;
    logos::logger("core").error("{}: refused after logos_core_start()", setter);
    return false;
}
} // namespace

int logos_core_set_core_service_transports(const char* transports_json) {
    if (!beforeStart("logos_core_set_core_service_transports")) return -1;
    logos::core_service::setTransports(transports_json ? transports_json : "");
    return 0;
}

int logos_core_set_shutdown_handler(LogosCoreShutdownHandler handler, void* user_data) {
    if (!beforeStart("logos_core_set_shutdown_handler")) return -1;
    logos::core_service::setShutdownHandler(handler, user_data);
    return 0;
}

int logos_core_set_operator_resolver(LogosCoreOperatorResolver resolver, void* user_data) {
    if (!beforeStart("logos_core_set_operator_resolver")) return -1;
    logos::core_service::setOperatorResolver(resolver, user_data);
    return 0;
}

int logos_core_set_core_service_extension(LogosCoreServiceExtension extension,
                                          const char* methods_json, void* user_data) {
    if (!beforeStart("logos_core_set_core_service_extension")) return -1;
    logos::core_service::setExtension(extension, methods_json ? methods_json : "", user_data);
    return 0;
}

int logos_core_set_shell_identity(const char* name) {
    if (!beforeStart("logos_core_set_shell_identity")) return -1;
    if (!name || !logos::isValidModuleName(name)) return -1;
    const auto* row = logos::bootstrap::rowFor(name);
    if (row) return -1; // the runtime's own modules are not shells
    logos::core_service::setShellIdentity(name);
    return 0;
}

void logos_core_start() {
    ModuleManager::markStarted();
    logos::initLogging();
    // Hosts inherit this value and therefore publish at the endpoint the
    // parent-side plain clients derive independently.
    ensureInstanceId();
    ModuleManager::anchorCoreApi();
    ModuleManager::discoverInstalledModules();
    ModuleManager::initializeCapabilityModule();
    // Admitted by capability when it is the authority, so modules can call it.
    logos::core_service::start();
    // After capability_module: this one is optional, and its snapshot back-fills
    // everything that happened before it was up.
    ModuleManager::initializeModulesState();
    logos::shell_binding::prepare();
}

void logos_core_cleanup() {
    ModuleManager::clear();
}

char** logos_core_get_loaded_modules() {
    return ModuleManager::getLoadedModulesCStr();
}

char** logos_core_get_known_modules() {
    return ModuleManager::getKnownModulesCStr();
}

int logos_core_load_module(const char* module_name, LogosLoadDeps deps) {
    if (!module_name) { logos::logger("core").critical("logos_core_load_module: module_name must not be null"); std::abort(); }
    // "Already loaded ⇒ success" is implemented in
    // ModuleManager::loadModuleInternal (see the block at the top there
    // for the rationale and the dep-tree fast path). The header doc
    // documents this as part of the public contract — keep both in sync.
    switch (deps) {
    case LOGOS_LOAD_REQUIRED_AND_OPTIONAL:
        return ModuleManager::loadModuleWithDependencies(
                   module_name, DependencyResolver::OptionalLoad::BestEffort) ? 1 : 0;
    case LOGOS_LOAD_REQUIRED_DEPS:
        return ModuleManager::loadModuleWithDependencies(
                   module_name, DependencyResolver::OptionalLoad::OrderOnly) ? 1 : 0;
    case LOGOS_LOAD_MODULE_ONLY:
        return ModuleManager::loadModule(module_name) ? 1 : 0;
    }
    // An out-of-range enum is a caller bug, and loading the required tree is
    // the answer that surprises least: it is what every caller of the old
    // `with_dependencies=true` asked for.
    logos::logger("core").warn("logos_core_load_module: unrecognised LogosLoadDeps {}; "
                               "treating as LOGOS_LOAD_REQUIRED_DEPS", static_cast<int>(deps));
    return ModuleManager::loadModuleWithDependencies(
               module_name, DependencyResolver::OptionalLoad::OrderOnly) ? 1 : 0;
}

char* logos_core_optional_load_report(const char* module_name) {
    if (!module_name) { logos::logger("core").critical("logos_core_optional_load_report: module_name must not be null"); std::abort(); }
    return ModuleManager::optionalLoadReportCStr(module_name);
}

int logos_core_unload_module(const char* module_name, bool with_dependents) {
    if (!module_name) { logos::logger("core").critical("logos_core_unload_module: module_name must not be null"); std::abort(); }
    if (with_dependents)
        return ModuleManager::unloadModuleWithDependents(module_name) ? 1 : 0;
    return ModuleManager::unloadModule(module_name) ? 1 : 0;
}

char** logos_core_get_module_dependencies(const char* module_name, bool recursive) {
    if (!module_name) { logos::logger("core").critical("logos_core_get_module_dependencies: module_name must not be null"); std::abort(); }
    return ModuleManager::getDependenciesCStr(module_name, recursive);
}

char** logos_core_get_module_optional_dependencies(const char* module_name) {
    if (!module_name) { logos::logger("core").critical("logos_core_get_module_optional_dependencies: module_name must not be null"); std::abort(); }
    return ModuleManager::getOptionalDependenciesCStr(module_name);
}

char** logos_core_get_module_dependents(const char* module_name, bool recursive) {
    if (!module_name) { logos::logger("core").critical("logos_core_get_module_dependents: module_name must not be null"); std::abort(); }
    return ModuleManager::getDependentsCStr(module_name, recursive);
}

char* logos_core_get_modules_info() {
    return ModuleManager::getModulesInfoCStr();
}

char* logos_core_process_module(const char* module_path) {
    if (!module_path) { logos::logger("core").critical("logos_core_process_module: module_path must not be null"); std::abort(); }
    return ModuleManager::processModuleCStr(module_path);
}

char* logos_core_get_token(const char* key) {
    if (!key) { logos::logger("core").critical("logos_core_get_token: key must not be null"); std::abort(); }

    char* stored = lp_token_get(key);
    std::string token = stored ? stored : "";
    lp_string_free(stored);
    if (token.empty()) return nullptr;

    char* result = new char[token.size() + 1];
    memcpy(result, token.c_str(), token.size() + 1);
    return result;
}

void logos_core_set_token_listener(LogosCoreTokenListener listener, void* user_data) {
    ModuleManager::setTokenListener(listener, user_data);
}

char* logos_core_get_module_stats() {
    return ProcessStats::getModuleStats(ModuleManager::getModuleProcessIds());
}

void logos_core_set_persistence_base_path(const char* path) {
    if (!path) { logos::logger("core").critical("logos_core_set_persistence_base_path: path must not be null"); std::abort(); }
    ModuleManager::setPersistenceBasePath(path);
}

void logos_core_set_module_transports(const char* module_name,
                                       const char* transport_set_json) {
    if (!module_name) {
        logos::logger("core").critical("logos_core_set_module_transports: module_name must not be null");
        std::abort();
    }
    ModuleManager::setModuleTransports(
        std::string(module_name),
        transport_set_json ? std::string(transport_set_json) : std::string{});
}

void logos_core_set_access_policy(const char* policy_json) {
    // NULL/"" clears the policy (see header) — unlike the module-name
    // setters above, this does not abort on NULL.
    ModuleManager::setAccessPolicy(
        policy_json ? std::string(policy_json) : std::string{});
}

void logos_core_refresh_modules()
{
    ModuleManager::discoverInstalledModules();
}
