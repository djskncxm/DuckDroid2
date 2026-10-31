// SPDX-License-Identifier: GPL-2.0-only
#include "common/log/logger.hpp"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace duckemu::log {
namespace {

// Warnings are diagnostics for pack maintainers; the shipped binaries keep
// stderr for errors only. A debugger can raise this back to Info/Warn with
// set_level() before creating the instance.
std::atomic<Level> g_level{Level::Error};
std::mutex g_mutex;

const char* level_name(Level level) noexcept {
    switch (level) {
    case Level::Trace:
        return "TRACE";
    case Level::Debug:
        return "DEBUG";
    case Level::Info:
        return "INFO";
    case Level::Warn:
        return "WARN";
    case Level::Error:
        return "ERROR";
    case Level::Off:
        return "OFF";
    }
    return "?";
}

std::string format_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()) %
                        1000;

    std::tm tm{};
    localtime_r(&seconds, &tm);

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec, static_cast<int>(millis.count()));
    return buffer;
}

std::string format_message(const char* fmt, std::va_list args)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 0)))
#endif
    ;

std::string format_message(const char* fmt, std::va_list args) {
    std::va_list copy;
    va_copy(copy, args);
    const int needed = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);

    if (needed <= 0) {
        return {};
    }

    std::string out(static_cast<size_t>(needed), '\0');
    std::vsnprintf(out.data(), out.size() + 1, fmt, args);
    return out;
}

} // namespace

void set_level(Level level) noexcept { g_level.store(level, std::memory_order_relaxed); }

Level level() noexcept { return g_level.load(std::memory_order_relaxed); }

void message(Level message_level, const char* file, int line, const char* fmt, ...) {
    if (message_level < g_level.load(std::memory_order_relaxed) || message_level == Level::Off) {
        return;
    }

    std::va_list args;
    va_start(args, fmt);
    std::string text = format_message(fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(stderr, "[%s] [%s] %s:%d: %s\n", format_timestamp().c_str(),
                 level_name(message_level), file, line, text.c_str());
}

} // namespace duckemu::log
