#pragma once
// Logging facade over spdlog (core.md §2.4, §3.5).
//
// Sinks: stdout (dev), optional rotating file, an in-memory console ring the editor drains each
// frame, and a breadcrumb ring (last 64 warnings/errors) flushed by the crash/assert handler.
// Hot-path rule: no MCE_LOG_* inside per-entity / per-draw loops; log a per-frame summary.

#include <spdlog/spdlog.h>

#include "mce/fixed_string.h"
#include "mce/types.h"

// The format is fmt's: "{}" placeholders ("{:.2f}", "{:08x}"), never printf's %s / %u. A literal
// format with a printf conversion fails to compile (it would log the % codes as they are).
#define MCE_LOG_TRACE(...) (static_cast<void>(::mce::log::detail::Format(MCE_LOG_FORMAT_(__VA_ARGS__, 0))), SPDLOG_TRACE(__VA_ARGS__))
#define MCE_LOG_DEBUG(...) (static_cast<void>(::mce::log::detail::Format(MCE_LOG_FORMAT_(__VA_ARGS__, 0))), SPDLOG_DEBUG(__VA_ARGS__))
#define MCE_LOG_INFO(...) (static_cast<void>(::mce::log::detail::Format(MCE_LOG_FORMAT_(__VA_ARGS__, 0))), SPDLOG_INFO(__VA_ARGS__))
#define MCE_LOG_WARN(...) (static_cast<void>(::mce::log::detail::Format(MCE_LOG_FORMAT_(__VA_ARGS__, 0))), SPDLOG_WARN(__VA_ARGS__))
#define MCE_LOG_ERROR(...) (static_cast<void>(::mce::log::detail::Format(MCE_LOG_FORMAT_(__VA_ARGS__, 0))), SPDLOG_ERROR(__VA_ARGS__))
#define MCE_LOG_CRITICAL(...) \
    (static_cast<void>(::mce::log::detail::Format(MCE_LOG_FORMAT_(__VA_ARGS__, 0))), SPDLOG_CRITICAL(__VA_ARGS__))
#define MCE_LOG_FORMAT_(f, ...) f

namespace mce::log::detail {

// Where a printf conversion starts in `s` (after %, flags, width, precision, length), or -1.
constexpr int printf_conversion(const char* s) {
    for (int i = 0; s[i] != 0; ++i) {
        if (s[i] != '%') continue;
        if (s[i + 1] == '%') {
            ++i;
            continue;
        }
        int j = i + 1;
        while (s[j] == '-' || s[j] == '+' || s[j] == '#' || s[j] == '0' || (s[j] >= '1' && s[j] <= '9') || s[j] == '.') ++j;
        while (s[j] == 'l' || s[j] == 'h' || s[j] == 'z' || s[j] == 'j' || s[j] == 't') ++j;
        for (const char* c = "sdiufFxXeEgGcp"; *c; ++c)
            if (s[j] == *c) return i;
    }
    return -1;
}

void printf_format_in_log_call();  // never defined: named in the error a printf format causes

// The first argument of a MCE_LOG_* call: a literal is checked while compiling; anything else
// (fmt::runtime(...)) passes as it is.
struct Format {
    template <usize N>
    consteval Format(const char (&s)[N]) {  // NOLINT(google-explicit-constructor)
        if (printf_conversion(s) >= 0) printf_format_in_log_call();
    }
    template <class T>
    constexpr Format(const T&) {}  // NOLINT(google-explicit-constructor)
};

}  // namespace mce::log::detail

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
