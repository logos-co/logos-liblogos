#include "logos_log.h"

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstdlib>
#include <memory>
#include <mutex>

namespace logos {
namespace {

// Uniform line format for every channel, e.g.
//   [2026-06-16 12:34:56.789] [warn] [subprocess] message
// %^...%$ colourises just the level; %n is the channel (logger) name, which
// replaces the old hand-typed "[SubprocessContainer]"-style prefixes.
constexpr const char* kPattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v";

// One shared stderr colour sink behind every channel. A single sink means one
// mutex serialising writes (no interleaved lines) and exactly one place
// writing to stderr.
std::shared_ptr<spdlog::sinks::sink>& sharedSink() {
    static std::shared_ptr<spdlog::sinks::sink> sink =
        std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    return sink;
}

} // namespace

void initLogging() {
    static std::once_flag once;
    std::call_once(once, []() {
        // Default logger ("logos") on the shared sink, so the existing
        // spdlog::info/warn/... free-function call sites share the same stream.
        spdlog::set_default_logger(
            std::make_shared<spdlog::logger>("logos", sharedSink()));

        // Registry-wide pattern: applied to every current logger and cloned
        // onto every logger created later via spdlog::initialize_logger (see
        // logger() below), so all channels format identically.
        spdlog::set_pattern(kPattern);

        // Honour LOGOS_LOG_LEVEL if set, else SPDLOG_LEVEL. spdlog parses the
        // per-channel syntax ("info,subprocess=warn") itself; an unset/empty
        // variable leaves the default (info) untouched.
        const char* lvlVar = (std::getenv("LOGOS_LOG_LEVEL") != nullptr)
                                 ? "LOGOS_LOG_LEVEL"
                                 : "SPDLOG_LEVEL";
        spdlog::cfg::load_env_levels(lvlVar);
    });
}

spdlog::logger& logger(const char* channel) {
    if (auto existing = spdlog::get(channel))
        return *existing;

    auto lg = std::make_shared<spdlog::logger>(channel, sharedSink());
    try {
        // initialize_logger (not register_logger) applies the registry-wide
        // pattern and the env-configured level for this channel before
        // registering it, so a channel created lazily after initLogging() is
        // still formatted and levelled correctly.
        spdlog::initialize_logger(lg);
    } catch (const spdlog::spdlog_ex&) {
        // Lost a race to register this name; use whoever won.
        if (auto existing = spdlog::get(channel))
            return *existing;
        throw;
    }
    return *lg;
}

} // namespace logos
