#ifndef LOGOS_LOGGING_LOG_H
#define LOGOS_LOGGING_LOG_H

#include <spdlog/spdlog.h>

// Standalone logging foundation for liblogos.
//
// Deliberately independent of every other subsystem — it depends only on
// spdlog. Lower-level components such as containers/ can log through it
// without taking a dependency on logos_core, so the dependency edge always
// points *down* into this foundation, never sideways into the core. (This is
// what lets containers/ be extracted to its own repo later.)
//
// The foundation is subsystem-agnostic: each caller names its own channel
// (e.g. "core", "subprocess"), which is printed as %n in every line.
//
// Runtime level control via LOGOS_LOG_LEVEL (falling back to SPDLOG_LEVEL):
//   LOGOS_LOG_LEVEL=debug                    # everything at debug
//   LOGOS_LOG_LEVEL="info,subprocess=warn"   # per-channel overrides
namespace logos {

// Initialise process-wide logging: a shared stderr colour sink, a uniform
// pattern, the shared default logger, and env-var level control. Idempotent
// and thread-safe — only the first call has any effect. Call once early, e.g.
// from logos_core_start().
void initLogging();

// Return the logger for the named channel, creating it (backed by the shared
// sink, with the uniform pattern and env-configured level) on first use.
// Safe to call before initLogging(); output then uses spdlog defaults until
// init runs.
spdlog::logger& logger(const char* channel);

} // namespace logos

#endif // LOGOS_LOGGING_LOG_H
