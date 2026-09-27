// A native module that answers `configuration` with the document it was given
// and whether that came before its context. Built once per FIXTURE_NAME, since a
// host checks an image's name, and once without the export
// (CONFIGURED_FIXTURE_NO_EXPORT), as an image that cannot be configured.
#include "logos_module_impl.h"
#include "logos_protocol.h"
#include "logos_runtime_delegate.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {

std::mutex mutex;
std::string configuration;
bool contextSet = false;
bool configuredBeforeContext = false;

char* copyResult(const std::string& value)
{
    char* result = static_cast<char*>(std::malloc(value.size() + 1));
    if (result) std::memcpy(result, value.c_str(), value.size() + 1);
    return result;
}

} // namespace

extern "C" {

char* logos_module_dispatch(const char* method, const char*)
{
    if (std::strcmp(method, "name") == 0) return copyResult("\"" FIXTURE_NAME "\"");
    if (std::strcmp(method, "configuration") == 0) {
        std::lock_guard<std::mutex> lock(mutex);
        return copyResult("{\"document\":" + (configuration.empty() ? "null" : configuration)
                          + ",\"before_context\":" + (configuredBeforeContext ? "true" : "false")
                          + "}");
    }
    return copyResult("\"ok\"");
}

char* logos_module_get_methods() { return copyResult("[]"); }

void logos_module_set_context(const char*, const char*, const char*)
{
    std::lock_guard<std::mutex> lock(mutex);
    contextSet = true;
}

#ifndef CONFIGURED_FIXTURE_NO_EXPORT
LOGOS_MODULE_IMPL_EXPORT int logos_module_set_configuration(const char* json)
{
    if (!json) return -1;
    std::lock_guard<std::mutex> lock(mutex);
    configuration = json;
    configuredBeforeContext = !contextSet;
    return 0;
}
#endif

// It calls no lp_*, so it runs in-process on any delegate.
LOGOS_MODULE_IMPL_EXPORT int logos_module_set_runtime_delegate(const lp_runtime_delegate_v1*)
{
    return 0;
}

void logos_module_set_emit_callback(logos_module_emit_cb, void*) {}
int logos_module_accept_token(const char*, const char*) { return 0; }
int logos_module_accept_inbound_token(const char*, const char*) { return 0; }
int logos_module_grant_host_services(const char*) { return 0; }
void logos_module_set_unload_done_callback(logos_module_unload_done_cb, void*) {}
int logos_module_about_to_unload() { return 0; }
void logos_module_set_call_caller(const char*) {}
const char* logos_module_get_protocol_version() { return LOGOS_PROTOCOL_VERSION_STRING; }
void logos_module_string_free(char* value) { std::free(value); }

} // extern "C"
