#include "mce/crash.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "mce/log.h"

#if defined(__EMSCRIPTEN__)
// A browser has no signals, minidumps or writable dump folder: a wasm trap surfaces as a JavaScript
// exception in the page's console (ADR-0039), so the handler is not installed there.
namespace mce::crash {
bool install(const CrashDesc&) { return false; }
bool installed() { return false; }
StringView dump_dir() { return {}; }
// The browser's virtual file system: saves live under /persistent, which M-WEB step 3 backs with IndexedDB.
FixedString<512> user_data_root() { return FixedString<512>("/persistent"); }
FixedString<512> default_dump_dir(StringView) { return {}; }
void trigger_test_crash() { std::abort(); }
// No crash report is written in a browser, so there is nothing for a context writer to add to.
bool add_context(ContextFn, void*) { return false; }
void remove_context(void*) {}
}  // namespace mce::crash
#else

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first: dbghelp.h needs its types.
#include <dbghelp.h>
#else
#include <dlfcn.h>
#if defined(__ANDROID__)
#include <unwind.h>  // bionic declares backtrace() only from API 33; the unwinder is always there
#else
#include <execinfo.h>
#endif
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/ucontext.h>  // <ucontext.h> needs _XOPEN_SOURCE on macOS
#elif defined(__linux__)
#include <link.h>
#include <ucontext.h>
#endif
#endif

#if defined(__SANITIZE_ADDRESS__)
#define MCE_CRASH_UNDER_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MCE_CRASH_UNDER_SANITIZER 1
#endif
#endif

namespace mce::crash {
namespace {

constexpr usize kMaxFrames = 64;

// Everything the handler needs, formatted by install(). Plain arrays: no allocation at crash time.
struct State {
    char dir[512] = {};
    char app[64] = {};
    char header[512] = {};
    usize header_len = 0;
#if defined(__linux__)
    char exe[512] = {};  // /proc/self/exe, for the main module (dl_iterate_phdr reports it as "")
#endif
    bool hooked = false;
};

State g_state;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_in_handler{false};

void copy_cstr(char* dst, usize cap, StringView src) {
    const usize n = src.size() < cap - 1 ? src.size() : cap - 1;
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

// A fixed buffer flushed to one file handle: hex/dec formatting without printf.
struct Out {
#if defined(_WIN32)
    HANDLE file = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
    char buf[2048];
    usize n = 0;

    void flush() {
        if (n == 0) return;
#if defined(_WIN32)
        DWORD written = 0;
        if (file != INVALID_HANDLE_VALUE) WriteFile(file, buf, static_cast<DWORD>(n), &written, nullptr);
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, static_cast<DWORD>(n), &written, nullptr);
#else
        if (fd >= 0) (void)!::write(fd, buf, n);
        (void)!::write(2, buf, n);
#endif
        n = 0;
    }
    void put(const char* s, usize len) {
        for (usize i = 0; i < len; ++i) {
            if (n == sizeof(buf)) flush();
            buf[n++] = s[i];
        }
    }
    void put(const char* s) { put(s, std::strlen(s)); }
    void hex(u64 v) {
        char t[18];
        usize k = 0;
        do {
            const u64 d = v & 0xF;
            t[k++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
            v >>= 4;
        } while (v != 0);
        put("0x", 2);
        while (k > 0) put(&t[--k], 1);
    }
    void dec(u64 v) {
        char t[20];
        usize k = 0;
        do {
            t[k++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v != 0);
        while (k > 0) put(&t[--k], 1);
    }
    void dec2(u64 v) {  // zero-padded to two digits
        if (v < 10) put("0", 1);
        dec(v);
    }
};

// UTC "YYYYMMDD-HHMMSS" from a time_t, by days-from-civil inverted (no gmtime: not async-signal-safe).
void put_stamp(Out& o, i64 t) {
    const i64 secs = t % 86400;
    i64 z = t / 86400 + 719468;
    const i64 era = (z >= 0 ? z : z - 146096) / 146097;
    const i64 doe = z - era * 146097;
    const i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const i64 mp = (5 * doy + 2) / 153;
    const i64 d = doy - (153 * mp + 2) / 5 + 1;
    const i64 m = mp < 10 ? mp + 3 : mp - 9;
    const i64 y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    o.dec(static_cast<u64>(y));
    o.dec2(static_cast<u64>(m));
    o.dec2(static_cast<u64>(d));
    o.put("-", 1);
    o.dec2(static_cast<u64>(secs / 3600));
    o.dec2(static_cast<u64>(secs / 60 % 60));
    o.dec2(static_cast<u64>(secs % 60));
}

// "<dir>/<app>-<stamp>-<pid><ext>" into path (the Out is only used as a formatter here).
void format_report_path(char* path, usize cap, u64 pid, const char* ext) {
    Out o;
    o.put(g_state.dir);
    o.put("/", 1);
    o.put(g_state.app);
    o.put("-", 1);
    put_stamp(o, static_cast<i64>(std::time(nullptr)));
    o.put("-", 1);
    o.dec(pid);
    o.put(ext);
    const usize n = o.n < cap - 1 ? o.n : cap - 1;
    std::memcpy(path, o.buf, n);
    path[n] = '\0';
}

void put_crumb(const log::ConsoleLine& line, void* user) {
    Out& o = *static_cast<Out*>(user);
    o.put("  ");
    o.put(line.text.c_str());
    o.put("\n");
}

// Writers registered with add_context: fn is stored last, so a half-written slot is never called.
constexpr usize kMaxContext = 4;
std::atomic<ContextFn> g_context_fn[kMaxContext];
std::atomic<void*> g_context_user[kMaxContext];

void put_context(Out& o) {
    bool any = false;
    for (usize i = 0; i < kMaxContext; ++i) {
        const ContextFn fn = g_context_fn[i].load(std::memory_order_acquire);
        if (!fn) continue;
        if (!any) o.put("context:\n");
        any = true;
        fn(g_context_user[i].load(std::memory_order_relaxed),
           [](void* sink, const char* text) { static_cast<Out*>(sink)->put(text); }, &o);
    }
}

void put_breadcrumbs(Out& o) {
    o.put("breadcrumbs (oldest first):\n");
    if (!log::try_visit_breadcrumbs(&put_crumb, &o)) o.put("  (log ring locked by the crashing thread)\n");
}

bool make_dirs(const char* path) {
    char tmp[512];
    copy_cstr(tmp, sizeof(tmp), StringView(path));
    for (char* p = tmp + 1; *p; ++p) {
        if (*p != '/' && *p != '\\') continue;
        const char c = *p;
        *p = '\0';
#if defined(_WIN32)
        CreateDirectoryA(tmp, nullptr);
#else
        ::mkdir(tmp, 0755);
#endif
        *p = c;
    }
#if defined(_WIN32)
    if (!CreateDirectoryA(tmp, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
#else
    if (::mkdir(tmp, 0755) != 0) {
        struct stat st {};
        if (::stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    }
#endif
    return true;
}

#if defined(_WIN32)
// ============================================================================ Windows

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDumpFn g_write_dump = nullptr;

void put_address(Out& o, u64 addr) {
    o.hex(addr);
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(addr)), &mod)) {
        char name[MAX_PATH];
        const DWORD len = GetModuleFileNameA(mod, name, MAX_PATH);
        o.put(" base ");
        o.hex(reinterpret_cast<uintptr_t>(mod));
        o.put(" +");
        o.hex(addr - reinterpret_cast<uintptr_t>(mod));
        o.put(" ");
        o.put(name, len);
    }
    o.put("\n");
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* ep) {
    if (g_in_handler.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
    const u64 pid = GetCurrentProcessId();
    char report[640];
    char dump[640];
    format_report_path(report, sizeof(report), pid, ".crash");
    format_report_path(dump, sizeof(dump), pid, ".dmp");

    bool dumped = false;
    if (g_write_dump) {
        HANDLE f = CreateFileA(dump, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{};
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers = FALSE;
            const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo);
            dumped = g_write_dump(GetCurrentProcess(), GetCurrentProcessId(), f, type, &mei, nullptr, nullptr) != FALSE;
            CloseHandle(f);
        }
    }

    Out o;
    o.file = CreateFileA(report, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    o.put(g_state.header, g_state.header_len);
    o.put("exception ");
    o.hex(ep->ExceptionRecord->ExceptionCode);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        o.put(" addr ");
        o.hex(ep->ExceptionRecord->ExceptionInformation[1]);
    }
    o.put("\npid ");
    o.dec(pid);
    o.put("\npc ");
    put_address(o, reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress));
    if (dumped) {
        o.put("minidump ");
        o.put(dump);
        o.put("\n");
    }
    void* frames[kMaxFrames];
    const USHORT n = CaptureStackBackTrace(0, static_cast<DWORD>(kMaxFrames), frames, nullptr);
    o.put("backtrace (the handler's own frames first; open the minidump for the faulting thread):\n");
    for (USHORT i = 0; i < n; ++i) {
        o.put("#");
        o.dec(i);
        o.put(" ");
        put_address(o, reinterpret_cast<uintptr_t>(frames[i]));
    }
    put_context(o);
    put_breadcrumbs(o);
    o.put("crash report: ");
    o.put(report);
    o.put("\n");
    o.flush();
    if (o.file != INVALID_HANDLE_VALUE) CloseHandle(o.file);
    return EXCEPTION_CONTINUE_SEARCH;
}

[[maybe_unused]] bool hook() {  // unused under ASan (see install)
    if (HMODULE dbghelp = LoadLibraryA("dbghelp.dll"))
        g_write_dump = reinterpret_cast<MiniDumpWriteDumpFn>(reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
    SetUnhandledExceptionFilter(&on_exception);
    return true;
}

#else
// ============================================================================ POSIX

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
constexpr usize kSignalCount = sizeof(kSignals) / sizeof(kSignals[0]);
// The handlers install() replaced (a crash vendor's, such as Crashlytics' NDK handler, installed
// before the engine's): after the report is written the crash is handed on to them.
struct sigaction g_prev[kSignalCount];
alignas(16) char g_alt_stack[64 * 1024];

const char* signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGFPE: return "SIGFPE";
        case SIGILL: return "SIGILL";
        case SIGABRT: return "SIGABRT";
        default: return "signal";
    }
}

u64 fault_pc(void* uctx) {
    if (!uctx) return 0;
    const auto* uc = static_cast<const ucontext_t*>(uctx);
#if defined(__APPLE__) && defined(__x86_64__)
    return static_cast<u64>(uc->uc_mcontext->__ss.__rip);
#elif defined(__APPLE__) && defined(__aarch64__) && defined(arm_thread_state64_get_pc)
    return static_cast<u64>(arm_thread_state64_get_pc(uc->uc_mcontext->__ss));
#elif defined(__APPLE__) && defined(__aarch64__)
    return static_cast<u64>(uc->uc_mcontext->__ss.__pc);
#elif defined(__linux__) && defined(__x86_64__)
    return static_cast<u64>(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__linux__) && defined(__aarch64__)
    return static_cast<u64>(uc->uc_mcontext.pc);
#else
    (void)uc;
    return 0;
#endif
}

#if defined(__APPLE__)
// "build_id <hex>" from the image's LC_UUID (what atos/dSYMs match on), or nothing.
void put_build_id(Out& o, const mach_header* mh) {
    const bool is64 = mh->magic == MH_MAGIC_64;
    if (!is64 && mh->magic != MH_MAGIC) return;
    const auto* p = reinterpret_cast<const unsigned char*>(mh) + (is64 ? sizeof(mach_header_64) : sizeof(mach_header));
    for (u32 i = 0; i < mh->ncmds; ++i) {
        const auto* lc = reinterpret_cast<const load_command*>(p);
        if (lc->cmd == LC_UUID) {
            const auto* uc = reinterpret_cast<const uuid_command*>(lc);
            static constexpr char kHex[] = "0123456789abcdef";
            o.put(" build_id ");
            for (unsigned char b : uc->uuid) {
                o.put(&kHex[b >> 4], 1);
                o.put(&kHex[b & 0xF], 1);
            }
            return;
        }
        p += lc->cmdsize;
    }
}

// One "module <load address> build_id <uuid> <path>" line per loaded image (dyld's list).
void put_modules(Out& o) {
    const u32 count = _dyld_image_count();
    for (u32 i = 0; i < count; ++i) {
        const mach_header* mh = _dyld_get_image_header(i);
        const char* name = _dyld_get_image_name(i);
        if (!mh) continue;
        o.put("module ");
        o.hex(reinterpret_cast<uintptr_t>(mh));
        put_build_id(o, mh);
        o.put(" ");
        o.put(name ? name : "?");
        o.put("\n");
    }
}
#elif defined(__linux__)
// "build_id <hex>" from the module's PT_NOTE segments (NT_GNU_BUILD_ID), or nothing.
void put_build_id(Out& o, const dl_phdr_info* info) {
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE) continue;
        const auto* p = reinterpret_cast<const unsigned char*>(info->dlpi_addr + ph.p_vaddr);
        const unsigned char* end = p + ph.p_memsz;
        while (p + sizeof(ElfW(Nhdr)) <= end) {
            const auto* nh = reinterpret_cast<const ElfW(Nhdr)*>(p);
            const unsigned char* name = p + sizeof(ElfW(Nhdr));
            const unsigned char* desc = name + ((nh->n_namesz + 3u) & ~3u);
            if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 && std::memcmp(name, "GNU", 4) == 0) {
                static constexpr char kHex[] = "0123456789abcdef";
                o.put(" build_id ");
                for (ElfW(Word) k = 0; k < nh->n_descsz; ++k) {
                    o.put(&kHex[desc[k] >> 4], 1);
                    o.put(&kHex[desc[k] & 0xF], 1);
                }
                return;
            }
            p = desc + ((nh->n_descsz + 3u) & ~3u);
        }
    }
}

int put_module(dl_phdr_info* info, size_t, void* user) {
    Out& o = *static_cast<Out*>(user);
    const char* name = info->dlpi_name && info->dlpi_name[0] ? info->dlpi_name : g_state.exe;
    if (std::strncmp(name, "linux-vdso", 10) == 0) return 0;
    o.put("module ");
    o.hex(info->dlpi_addr);
    put_build_id(o, info);
    o.put(" ");
    o.put(name);
    o.put("\n");
    return 0;
}

void put_modules(Out& o) { dl_iterate_phdr(&put_module, &o); }
#else
void put_modules(Out&) {}  // no portable module list on other POSIX systems
#endif

#if defined(__ANDROID__)
struct UnwindState {
    void** frames;
    int max;
    int n;
};

_Unwind_Reason_Code unwind_frame(_Unwind_Context* ctx, void* user) {
    auto* st = static_cast<UnwindState*>(user);
    const uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc != 0) {
        if (st->n >= st->max) return _URC_END_OF_STACK;
        st->frames[st->n++] = reinterpret_cast<void*>(pc);
    }
    return _URC_NO_REASON;
}

// execinfo's backtrace() over the unwinder: same contract (return addresses, innermost first).
int backtrace(void** frames, int max) {
    UnwindState st{frames, max, 0};
    _Unwind_Backtrace(&unwind_frame, &st);
    return st.n;
}
#endif

void put_frame(Out& o, usize index, u64 pc) {
    o.put("#");
    o.dec(index);
    o.put(" ");
    o.hex(pc);
    Dl_info di{};
    if (dladdr(reinterpret_cast<void*>(static_cast<uintptr_t>(pc)), &di) && di.dli_fbase) {
        const u64 base = reinterpret_cast<uintptr_t>(di.dli_fbase);
        o.put(" base ");
        o.hex(base);
        o.put(" +");
        o.hex(pc - base);
        if (di.dli_sname) {
            o.put(" ");
            o.put(di.dli_sname);
        }
    }
    o.put("\n");
}

void on_signal(int sig, siginfo_t* info, void* uctx) {
    if (g_in_handler.exchange(true)) _exit(128 + sig);  // crashed while reporting a crash
    const u64 pid = static_cast<u64>(::getpid());
    char report[640];
    format_report_path(report, sizeof(report), pid, ".crash");

    Out o;
    o.fd = ::open(report, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    o.put(g_state.header, g_state.header_len);
    o.put("signal ");
    o.dec(static_cast<u64>(sig));
    o.put(" (");
    o.put(signal_name(sig));
    o.put(") code ");
    o.dec(static_cast<u64>(static_cast<u32>(info ? info->si_code : 0)));
    o.put(" addr ");
    o.hex(info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0);
    o.put("\npid ");
    o.dec(pid);
    o.put("\n");
    const u64 pc = fault_pc(uctx);
    o.put("pc ");
    o.hex(pc);
    o.put("\n");

    // The unwinder walks through the signal frame; the frames above the faulting pc are this handler's.
    void* frames[kMaxFrames];
    const int n = backtrace(frames, static_cast<int>(kMaxFrames));
    int first = 0;
    for (int i = 0; i < n; ++i) {
        if (reinterpret_cast<uintptr_t>(frames[i]) == pc) {
            first = i;
            break;
        }
    }
#if defined(__APPLE__)
    // Darwin's frame-pointer walk skips the faulting frame itself: it goes from _sigtramp straight to
    // the caller. Report the pc as #0 and continue from the frame after _sigtramp.
    bool pc_first = false;
    if (first == 0 && pc != 0 && (n == 0 || reinterpret_cast<uintptr_t>(frames[0]) != pc)) {
        for (int i = 0; i < n; ++i) {
            Dl_info di{};
            if (dladdr(frames[i], &di) && di.dli_sname && std::strcmp(di.dli_sname, "_sigtramp") == 0) {
                first = i + 1;
                pc_first = true;
                break;
            }
        }
        // The next entry can be a stale link register inside the faulting function itself (its last
        // call's return address), not a caller: drop it when it lands in the same symbol before pc.
        Dl_info at_pc{};
        Dl_info next{};
        if (pc_first && first < n && dladdr(reinterpret_cast<void*>(static_cast<uintptr_t>(pc)), &at_pc) &&
            dladdr(frames[first], &next) && at_pc.dli_saddr && at_pc.dli_saddr == next.dli_saddr &&
            reinterpret_cast<uintptr_t>(frames[first]) < pc)
            ++first;
    }
#else
    constexpr bool pc_first = false;
#endif
    o.put("backtrace:\n");
    usize index = 0;
    if (pc_first) put_frame(o, index++, pc);
    for (int i = first; i < n; ++i) put_frame(o, index++, reinterpret_cast<uintptr_t>(frames[i]));
    put_modules(o);
    put_context(o);
    put_breadcrumbs(o);
    o.put("crash report: ");
    o.put(report);
    o.put("\n");
    o.flush();
    if (o.fd >= 0) ::close(o.fd);

    // Hand the signal to the handler that was there before install() (a crash vendor's), or die of it
    // with the default action (core dump, the parent sees the signal). It is raised again: blocked
    // while this handler runs, it is delivered to that handler as soon as this one returns.
    struct sigaction next {};
    next.sa_handler = SIG_DFL;
    sigemptyset(&next.sa_mask);
    for (usize i = 0; i < kSignalCount; ++i) {
        const bool has_handler = (g_prev[i].sa_flags & SA_SIGINFO) != 0 ? g_prev[i].sa_sigaction != nullptr
                                                                        : (g_prev[i].sa_handler != SIG_DFL &&
                                                                           g_prev[i].sa_handler != SIG_IGN);
        if (kSignals[i] == sig && has_handler) next = g_prev[i];
    }
    sigaction(sig, &next, nullptr);
    raise(sig);
}

[[maybe_unused]] bool hook() {  // unused under ASan (see install)
#if defined(__linux__)
    const ssize_t len = ::readlink("/proc/self/exe", g_state.exe, sizeof(g_state.exe) - 1);
    g_state.exe[len > 0 ? len : 0] = '\0';
#endif
    void* warm[1];
    (void)backtrace(warm, 1);  // loads the unwinder now, not inside the handler

    stack_t ss{};
    ss.ss_sp = g_alt_stack;
    ss.ss_size = sizeof(g_alt_stack);
    if (sigaltstack(&ss, nullptr) != 0) return false;
    struct sigaction sa {};
    sa.sa_sigaction = &on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (usize i = 0; i < kSignalCount; ++i) {
        struct sigaction prev {};
        if (sigaction(kSignals[i], &sa, &prev) == 0 && prev.sa_sigaction != &on_signal) g_prev[i] = prev;
    }
    return true;
}
#endif

}  // namespace

FixedString<512> user_data_root() {
    FixedString<512> r;
#if defined(_WIN32)
    const char* base = std::getenv("LOCALAPPDATA");
    r.assign(base ? base : ".");
#elif defined(__ANDROID__)
    // An app has no HOME: its private files directory, from the package name in /proc/self/cmdline.
    char pkg[256] = {};
    const int fd = ::open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        const ssize_t n = ::read(fd, pkg, sizeof(pkg) - 1);
        pkg[n > 0 ? n : 0] = '\0';
        ::close(fd);
    }
    r.assign("/data/data/");
    r.append(pkg);
    r.append("/files");
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    // The app sandbox: only Documents, Library and tmp are writable.
    const char* home = std::getenv("HOME");
    r.assign(home ? home : ".");
    r.append("/Library/Application Support");
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    if (xdg && *xdg) {
        r.assign(xdg);
    } else {
        r.assign(home ? home : ".");
        r.append("/.local/share");
    }
#endif
    return r;
}

FixedString<512> default_dump_dir(StringView app_name) {
    FixedString<512> r = user_data_root();
    r.append("/MCE/");
    r.append(app_name);
    r.append("/crashes");
    return r;
}

bool install(const CrashDesc& desc) {
    const StringView app(desc.app_name ? desc.app_name : "mce");
    FixedString<512> dir;
    if (desc.dump_dir && *desc.dump_dir)
        dir.assign(desc.dump_dir);
    else
        dir = default_dump_dir(app);
    if (!make_dirs(dir.c_str())) {
        MCE_LOG_WARN("crash: cannot create the report directory {}", dir.c_str());
        return false;
    }
    copy_cstr(g_state.dir, sizeof(g_state.dir), dir.view());
    copy_cstr(g_state.app, sizeof(g_state.app), app);

    Out h;  // formatter only
    h.put("MCE crash report v1\napp ");
    h.put(g_state.app);
    h.put("\nversion ");
    h.put(desc.version && *desc.version ? desc.version : "unknown");
    h.put("\n");
    g_state.header_len = h.n < sizeof(g_state.header) ? h.n : sizeof(g_state.header);
    std::memcpy(g_state.header, h.buf, g_state.header_len);

#if defined(MCE_CRASH_UNDER_SANITIZER)
    // The sanitizer's own SEGV report is the better one in those builds; leave its handlers alone.
    return true;
#else
    if (!g_state.hooked) g_state.hooked = hook();
    g_installed.store(g_state.hooked);
    return g_state.hooked;
#endif
}

bool installed() { return g_installed.load(); }

StringView dump_dir() { return StringView(g_state.dir); }

void trigger_test_crash() {
    MCE_LOG_ERROR("crash: -crash_test requested, dereferencing null");
    volatile int* volatile p = nullptr;
    *p = 42;  // NOLINT(clang-analyzer-core.NullDereference): the point
    std::abort();
}

bool add_context(ContextFn fn, void* user) {
    for (usize i = 0; i < kMaxContext; ++i) {
        ContextFn expected = nullptr;
        if (g_context_fn[i].load(std::memory_order_relaxed) != nullptr) continue;
        g_context_user[i].store(user, std::memory_order_relaxed);
        if (g_context_fn[i].compare_exchange_strong(expected, fn, std::memory_order_release)) return true;
    }
    return false;
}

void remove_context(void* user) {
    for (usize i = 0; i < kMaxContext; ++i)
        if (g_context_fn[i].load(std::memory_order_relaxed) && g_context_user[i].load(std::memory_order_relaxed) == user)
            g_context_fn[i].store(nullptr, std::memory_order_release);
}

}  // namespace mce::crash

#endif  // !__EMSCRIPTEN__
