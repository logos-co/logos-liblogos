// A stand-in peering_module for PeeringLink.AnAppMayExitWithPeeringLive: a native
// module that answers configure, so peering_link starts, and imports, slowly, so
// peering_link's first job is still running when the app exits.
#include "logos_module_impl.h"
#include "logos_runtime_delegate.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

#define STUB_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

char* copy(const char* text)
{
    auto* out = static_cast<char*>(std::malloc(std::strlen(text) + 1));
    std::strcpy(out, text);
    return out;
}

} // namespace

STUB_EXPORT char* logos_module_dispatch(const char* method, const char*)
{
    if (!method) return nullptr;
    if (std::strcmp(method, "name") == 0) return copy("\"peering_module\"");
    if (std::strcmp(method, "configure") == 0) {
        std::printf("CONFIGURED\n");
        std::fflush(stdout);
        return copy(R"({"runtime_id":"00000000-0000-4000-8000-000000000001"})");
    }
    if (std::strcmp(method, "imports") == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return copy("{}");
    }
    return nullptr;
}

STUB_EXPORT char* logos_module_get_methods(void)
{
    return copy(R"([{"name":"configure","type":"method","returnType":"LogosMap",)"
                R"("isInvokable":true,"parameters":[{"name":"config","type":"LogosMap"}]},)"
                R"({"name":"imports","type":"method","returnType":"LogosMap",)"
                R"("isInvokable":true,"parameters":[]}])");
}

STUB_EXPORT void logos_module_set_context(const char*, const char*, const char*) {}
STUB_EXPORT void logos_module_set_emit_callback(logos_module_emit_cb, void*) {}
STUB_EXPORT int logos_module_accept_token(const char*, const char*) { return LP_OK; }
STUB_EXPORT int logos_module_accept_inbound_token(const char*, const char*) { return LP_OK; }
STUB_EXPORT int logos_module_grant_host_services(const char*) { return LP_OK; }
STUB_EXPORT void logos_module_set_unload_done_callback(logos_module_unload_done_cb, void*) {}
STUB_EXPORT int logos_module_about_to_unload(void) { return 0; }
STUB_EXPORT void logos_module_set_call_caller(const char*) {}
STUB_EXPORT const char* logos_module_get_protocol_version(void) { return LOGOS_PROTOCOL_VERSION_STRING; }
STUB_EXPORT void logos_module_string_free(char* text) { std::free(text); }
STUB_EXPORT int logos_module_set_runtime_delegate(const lp_runtime_delegate_v1*) { return LP_OK; }
