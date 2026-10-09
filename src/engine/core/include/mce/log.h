#pragma once
// Logging facade over spdlog (core.md §2.4, §3.5).
//
// Sinks: stdout (dev), optional rotating file, an in-memory console ring the editor drains each
// frame, and a breadcrumb ring (last 64 warnings/errors) flushed by the crash/assert handler.
// Hot-path rule: no MCE_LOG_* inside per-entity / per-draw loops; log a per-frame summary.

#include <spdlog/spdlog.h>

#include "mce/fixed_string.h"
#include "mce/types.h"

#define MCE_LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define MCE_LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define MCE_LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define MCE_LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define MCE_LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define MCE_LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)

namespace mce::log {

enum class Level : u8 { Trace, Debug, Info, Warn, Error, Critical, Off };

struct LogDesc {
    const char* app_name = "mce";
    const char* file_path = nullptr;  // rotating file sink when set
    Level level = Level::Info;
    bool console = true;              // stdout sink
};

// Installs the MCE sinks as spdlog's default logger. Safe to call more than once (re-inits).
void init(const LogDesc& desc = {});
void shutdown();
void set_level(Level level);
void flush();

struct ConsoleLine {
    u64 seq = 0;  // monotonically increasing, starts at 1
    Level level = Level::Info;
    FixedString<256> text;
};

inline constexpr usize kConsoleRingSize = 1024;
inline constexpr usize kBreadcrumbCount = 64;

// Copies console lines with seq > since into out (oldest first). Returns the count written.
usize read_console(u64 since, Span<ConsoleLine> out);
u64 console_last_seq();

// Visits the breadcrumb ring, oldest first.
using BreadcrumbFn = void (*)(const ConsoleLine& line, void* user);
void visit_breadcrumbs(BreadcrumbFn fn, void* user);
// Same, but gives up (returns false) instead of waiting when the ring is locked: the crash handler
// may run on a thread that died holding it.
bool try_visit_breadcrumbs(BreadcrumbFn fn, void* user);

}  // namespace mce::log
