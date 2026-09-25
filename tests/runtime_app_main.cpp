// A stand-in app for the runtime-process tests: spawns the runtime with the
// configuration in argv[1], prints its pid and its module hosts' pids, and stays
// up until it is killed.
#include "logos_core.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

int main(int argc, char* argv[])
{
    if (argc < 2) return 2;
    char* error = nullptr;
    logos_runtime* runtime = logos_runtime_spawn(argv[1], &error);
    if (!runtime) {
        std::printf("SPAWN_FAILED %s\n", error ? error : "");
        std::fflush(stdout);
        return 1;
    }
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
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}
