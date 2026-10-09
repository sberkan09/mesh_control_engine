#pragma once
// Crash reports for shipped builds (packaging v2, ROADMAP M8).
//
// install() hooks the fatal signals (POSIX: SEGV, BUS, FPE, ILL, ABRT, on an alternate stack) or the
// unhandled-exception filter (Windows). A crash writes `<dump_dir>/<app>-<UTC stamp>-<pid>.crash`, a
// plain-text report: the app and version line, the signal, the faulting pc, a backtrace as
// module + offset, every loaded module with its base and GNU build id, and the log breadcrumbs.
// Windows also writes a `.dmp` minidump next to it (dbghelp, loaded at install). Shipped binaries
// are stripped; scripts/symbolicate.py resolves a report against the symbol archive ship.py keeps
// beside the zip (debug files keyed by build id), so no symbols ever leave the studio.
//
// The handler itself only uses what install() prepared: the paths and header text are formatted up
// front, the report is written with raw file calls, and the breadcrumb ring is read with a try-lock.

#include "mce/fixed_string.h"
#include "mce/types.h"

namespace mce::crash {

struct CrashDesc {
    const char* app_name = "mce";
    const char* version = nullptr;   // free text for the report header (e.g. "1.2.0+abc123 linux-gcc-release")
    const char* dump_dir = nullptr;  // created when missing; nullptr = default_dump_dir(app_name)
};

// Idempotent: a second call replaces the description (paths, header) and keeps the hooks.
// Returns false when the dump directory cannot be created; the hooks stay off then.
bool install(const CrashDesc& desc);
bool installed();

// The directory reports go to (empty until install()).
StringView dump_dir();

// Per-user: %LOCALAPPDATA%/MCE/<app>/crashes, $XDG_DATA_HOME (or ~/.local/share)/MCE/<app>/crashes.
FixedString<512> default_dump_dir(StringView app_name);

// Dereferences null, for `-crash_test 1` and the crash-report test: proves the whole chain.
[[noreturn]] void trigger_test_crash();

}  // namespace mce::crash
