#ifndef LOGOS_LOGGING_H
#define LOGOS_LOGGING_H

#include <spdlog/spdlog.h>

// Logging facade: call sites use logos_log_* only. To change backends, adjust
// this file (or add logos_log.cpp with non-inline implementations).

#define logos_log_info(...) ::spdlog::info(__VA_ARGS__)
#define logos_log_warn(...) ::spdlog::warn(__VA_ARGS__)
#define logos_log_error(...) ::spdlog::error(__VA_ARGS__)
#define logos_log_critical(...) ::spdlog::critical(__VA_ARGS__)

#endif
