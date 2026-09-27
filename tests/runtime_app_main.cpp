// A stand-in app for the runtime-process tests: spawns the runtime with the
// configuration in argv[1], prints its pid and its module hosts' pids, and stays
// up until it is killed. With "exit" in argv[2] it calls exit(1) instead, as an
// app's error path does: the runtime live and an event subscription armed. With
// "embed" it runs the runtime in its own process and reports on that instead.
#include "logos_core.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <string>
#include <thread>
#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

// Destroyed after the statics the libraries made while running: time for any
// thread woken during exit to reach one of them.
struct SlowTeardown {
    bool armed = false;
    ~SlowTeardown()
    {
        if (armed) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
} g_teardown;

// What it serves, whether it bound a socket in TMPDIR or has a child, and that
// neither a second start nor a spawn is let in once it has run.
int embed(const char* config)
{
    char* error = nullptr;
    logos_runtime* runtime = logos_runtime_embed(config, &error);
    if (!runtime) {
        std::printf("EMBED_FAILED %s\n", error ? error : "");
        std::fflush(stdout);
        return 1;
    }
    char* result = nullptr;
    char* failure = nullptr;
    logos_consumer_call(logos_runtime_binding(runtime), "core_service", "listModules", "[\"all\"]",
                        10000, &result, &failure);
    std::printf("MODULES %s\n", result ? result : "null");
    logos_consumer_string_free(result);
    logos_consumer_string_free(failure);
    int sockets = 0;
    std::error_code ec;
    const char* tmp = std::getenv("TMPDIR");
    for (std::filesystem::recursive_directory_iterator it(tmp ? tmp : "/tmp", ec), end;
         !ec && it != end; it.increment(ec))
        if (it->is_socket(ec)) ++sockets;
    std::printf("SOCKETS %d\n", sockets);
#ifndef _WIN32
    const bool children = !(waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD);
    std::printf("CHILDREN %d\n", children ? 1 : 0);
#endif
    logos_runtime_stop(runtime);
    char* again = nullptr;
    std::printf("SECOND_EMBED %s\n", logos_runtime_embed(config, &again) ? "accepted" : "refused");
    logos_consumer_string_free(again);
    char* spawnError = nullptr;
    std::printf("SPAWN_AFTER %s\n", logos_runtime_spawn(config, &spawnError) ? "accepted" : "refused");
    logos_consumer_string_free(spawnError);
    std::printf("READY\n");
    std::fflush(stdout);
    std::_Exit(0);
}

// Embedded, then exit(1) with it live and a subscription armed: its threads are
// this process's now, and exit runs the libraries' static destructors under them.
int embedThenExit(const char* config)
{
    char* error = nullptr;
    logos_runtime* runtime = logos_runtime_embed(config, &error);
    if (!runtime) {
        std::printf("EMBED_FAILED %s\n", error ? error : "");
        std::fflush(stdout);
        return 2;
    }
    logos_consumer_subscribe(logos_runtime_binding(runtime), "core_service", "moduleStateChanged",
                             [](const char*, const char*, void*) {}, nullptr);
    std::printf("READY\n");
    std::fflush(stdout);
    g_teardown.armed = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // the subscription arms
    std::exit(1);
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 2) return 2;
    if (argc > 2 && std::string(argv[2]) == "embed") return embed(argv[1]);
    if (argc > 2 && std::string(argv[2]) == "embed-exit") return embedThenExit(argv[1]);
    const bool exitLive = argc > 2 && std::string(argv[2]) == "exit";
    char* error = nullptr;
    logos_runtime* runtime = logos_runtime_spawn(argv[1], &error);
    if (!runtime) {
        std::printf("SPAWN_FAILED %s\n", error ? error : "");
        std::fflush(stdout);
        return 1;
    }
    if (exitLive)
        logos_consumer_subscribe(logos_runtime_binding(runtime), "core_service", "moduleStateChanged",
                                 [](const char*, const char*, void*) {}, nullptr);
    auto call = [&](const char* method) {
        char* result = nullptr;
        char* failure = nullptr;
        logos_consumer_call(logos_runtime_binding(runtime), "core_service", method, "[]", 10000,
                            &result, &failure);
        const auto value = nlohmann::json::parse(result ? result : "null", nullptr, false);
        logos_consumer_string_free(result);
        logos_consumer_string_free(failure);
        return value;
    };
    const auto status = call("getStatus");
    if (status.is_object() && status.contains("daemon"))
        std::printf("RUNTIME_PID %lld\n", status["daemon"].value("pid", 0LL));
    const auto stats = call("getModuleStats");
    if (stats.is_array())
        for (const auto& module : stats)
            if (module.is_object())
                std::printf("HOST_PID %lld\n", module.value("pid", 0LL));
    std::printf("READY\n");
    std::fflush(stdout);
    if (exitLive) {
        g_teardown.armed = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // the subscription arms
        std::exit(1);
    }
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}
