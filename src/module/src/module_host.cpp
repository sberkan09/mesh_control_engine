#include "mce/module_host.h"

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "mce/log.h"
#include "mce/platform/library.h"
#include "mce/refl/type.h"

namespace mce {
namespace {

// Copy-and-load slots. Recompile-between-Plays needs 2 per module; tests load a few at once.
constexpr u32 kSlotCount = 8;

struct Slot {
    bool used = false;
    u32 worlds_alive = 0;  // Worlds the module was installed into that still exist
    bool owns_copy = false;  // loaded_path is our copy (deleted on release), not the source mapped in place
    FixedString<512> loaded_path;
    // The reflection records the module's static registrars added. unload_game_module drops them
    // itself: the loader may keep a C++ module mapped after dlclose (STB_GNU_UNIQUE symbols), and
    // then its registrars never run their destructors, so the old field lists would shadow the
    // rebuilt module's (a component that gained a field would refuse it).
    u32 type_count = 0;
    u32 enum_count = 0;
    const refl::TypeRecord* types[256] = {};
    const refl::EnumRecord* enums[64] = {};
};
Slot g_slots[kSlotCount];

template <class Rec>
bool contains(Span<const Rec* const> list, const Rec* r) {
    for (const Rec* x : list)
        if (x == r) return true;
    return false;
}

// Remembers what appeared in `now` but not in `before` (the module's own records).
template <class Rec, usize N>
void remember_new(Span<const Rec* const> now, const Rec* const* before, u32 before_count, const Rec* (&out)[N], u32& count) {
    for (const Rec* r : now) {
        if (contains(Span<const Rec* const>(before, before_count), r)) continue;
        MCE_ASSERT_MSG(count < N, "game module registers too many reflected records");
        if (count < N) out[count++] = r;
    }
}

// Lives in the World as a resource: its destructor tells the loader the World is gone, so
// unload_game_module() can assert the teardown order (World first, then the library).
struct ModuleWorldBinding {
    u32 slot = ~0u;

    explicit ModuleWorldBinding(u32 s) : slot(s) {}
    ModuleWorldBinding(ModuleWorldBinding&& o) noexcept : slot(o.slot) { o.slot = ~0u; }
    ModuleWorldBinding(const ModuleWorldBinding&) = delete;
    ModuleWorldBinding& operator=(const ModuleWorldBinding&) = delete;
    ModuleWorldBinding& operator=(ModuleWorldBinding&& o) noexcept {
        if (this != &o) {
            release();
            slot = o.slot;
            o.slot = ~0u;
        }
        return *this;
    }
    ~ModuleWorldBinding() { release(); }

    void release() {
        if (slot < kSlotCount && g_slots[slot].worlds_alive > 0) --g_slots[slot].worlds_alive;
        slot = ~0u;
    }
};

u32 process_id() {
#if defined(_WIN32)
    return static_cast<u32>(GetCurrentProcessId());
#else
    return static_cast<u32>(getpid());
#endif
}

// "<dir>/libgame.so" + 1 -> "<dir>/libgame_loaded_<pid>_1.so". The process id keeps two processes
// mapping the same module apart (the editor and the running game): copy_in_use only knows this
// process's copies, and overwriting a copy another process has mapped corrupts its code.
FixedString<512> copy_path(StringView source, u32 n) {
    usize sep = 0;  // one past the last path separator
    for (usize i = 0; i < source.size(); ++i)
        if (source[i] == '/' || source[i] == '\\') sep = i + 1;
    usize dot = source.size();
    for (usize i = source.size(); i > sep; --i) {
        if (source[i - 1] == '.') {
            dot = i - 1;
            break;
        }
    }
    char suffix[40];
    std::snprintf(suffix, sizeof suffix, "_loaded_%u_%u", process_id(), n);
    FixedString<512> out(source.substr(0, dot));
    out.append(suffix);
    out.append(source.substr(dot));
    return out;
}

bool copy_in_use(StringView path) {
    for (const Slot& s : g_slots)
        if (s.used && s.loaded_path == path) return true;
    return false;
}

void release_slot(u32 slot) {
    Slot& s = g_slots[slot];
    if (s.owns_copy) std::remove(s.loaded_path.c_str());
    s = Slot{};
}

}  // namespace

Result<LoadedModule> open_game_module(StringView path) {
    if (!platform::file_exists(path)) return Error(Errc::ModuleNotFound, "module.open", path);

    u32 slot = kSlotCount;
    for (u32 i = 0; i < kSlotCount; ++i) {
        if (!g_slots[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == kSlotCount) return Error(Errc::Unsupported, "module.copy", path, kSlotCount);

    // A copy name this process has never used: a rebuilt module never lands on a mapped image, and
    // the dynamic loader never hands back an image it kept for a name it saw before (dlclose may
    // leave a C++ module mapped, e.g. for its STB_GNU_UNIQUE symbols, and then dlopen of the same
    // path returns the old code).
    static u32 next_copy = 0;
    u32 n = next_copy;
    while (copy_in_use(copy_path(path, n).view())) ++n;
    next_copy = n + 1;
    LoadedModule m;
    m.slot = slot;
    m.source_path = path;
    m.loaded_path = copy_path(path, n);
    if (m.source_path.truncated() || m.loaded_path.truncated())
        return Error(Errc::InvalidArgument, "module.copy", path);

    // A read-only install (a translocated or /Applications .app, a read-only Program Files or /opt)
    // cannot take the copy: map the module in place. Only rebuilding it while mapped needs the copy,
    // and that happens in a writable build tree.
    const bool copied = platform::copy_file(path, m.loaded_path).has_value();
    if (!copied) {
        MCE_LOG_INFO("module: cannot copy {} next to itself (read-only folder?); mapping it in place", path);
        m.loaded_path = path;
    }
    g_slots[slot].used = true;
    g_slots[slot].owns_copy = copied;
    g_slots[slot].loaded_path = m.loaded_path;

    // Snapshot the registry: whatever the module's static initialisers add is the module's.
    static const refl::TypeRecord* types_before[1024];
    static const refl::EnumRecord* enums_before[256];
    const Span<const refl::TypeRecord* const> t0 = refl::all();
    const Span<const refl::EnumRecord* const> e0 = refl::all_enums();
    const u32 nt = static_cast<u32>(t0.size() < 1024 ? t0.size() : 1024);
    const u32 ne = static_cast<u32>(e0.size() < 256 ? e0.size() : 256);
    for (u32 i = 0; i < nt; ++i) types_before[i] = t0[i];
    for (u32 i = 0; i < ne; ++i) enums_before[i] = e0[i];
    const Result<void*> lib = platform::load_library(m.loaded_path);
    if (!lib) {
        release_slot(slot);
        return Error(Errc::ModuleNotFound, "module.open", path);
    }
    m.handle = lib.value();
    remember_new(refl::all(), types_before, nt, g_slots[slot].types, g_slots[slot].type_count);
    remember_new(refl::all_enums(), enums_before, ne, g_slots[slot].enums, g_slots[slot].enum_count);

    const auto entry = reinterpret_cast<ModuleEntryFn>(platform::library_symbol(m.handle, kModuleEntrySymbol));
    m.info = entry ? entry() : nullptr;
    if (m.info == nullptr || m.info->install == nullptr) {
        platform::unload_library(m.handle);
        release_slot(slot);
        return Error(Errc::ModuleMissingEntry, "module.entry", path);
    }
    if (m.info->engine_id != MCE_ENGINE_ID) {
        MCE_LOG_ERROR("module: {} was built against engine {:016x}, this engine is {:016x} ({}); rebuild it",
                      path, m.info->engine_id, MCE_ENGINE_ID, MCE_ENGINE_DESCRIBE);
        platform::unload_library(m.handle);
        release_slot(slot);
        return Error(Errc::EngineIdMismatch, "module.version", path);
    }
    MCE_LOG_INFO("module: loaded {} v{} from {} (engine {:016x})", m.info->name, m.info->module_version,
                 m.loaded_path.c_str(), MCE_ENGINE_ID);
    return m;
}

ResultV install_game_module(const LoadedModule& m, World& world) {
    if (!m.loaded() || m.slot >= kSlotCount || !g_slots[m.slot].used)
        return Error(Errc::InvalidArgument, "module.install", "module not loaded");
    if (world.try_resource<ModuleWorldBinding>() != nullptr)
        return Error(Errc::AlreadyExists, "module.install", m.source_path.view());
    ++g_slots[m.slot].worlds_alive;
    world.set_resource(ModuleWorldBinding(m.slot));
    const ResultV installed = m.info->install(world);
    if (!installed) {
        FixedString<512> msg;
        format_error(installed.error(), msg);
        // The World may already hold code pointers into the module: it stays mapped (and its
        // slot taken) until the World is gone. The caller discards the World.
        MCE_LOG_ERROR("module: {} install failed: {}", m.info->name, msg.c_str());
        return Error(Errc::ModuleInstallFailed, "module.install", m.source_path.view());
    }
    return ok();
}

Result<LoadedModule> load_game_module(StringView path, World& world) {
    if (world.try_resource<ModuleWorldBinding>() != nullptr)
        return Error(Errc::AlreadyExists, "module.install", path);
    MCE_TRY_ASSIGN(LoadedModule m, open_game_module(path));
    MCE_TRY(install_game_module(m, world));
    return m;
}

void unload_game_module(LoadedModule& module) {
    if (!module.loaded()) return;
    MCE_ASSERT_MSG(module.slot < kSlotCount && g_slots[module.slot].used, "unknown module slot");
    MCE_ASSERT_MSG(g_slots[module.slot].worlds_alive == 0,
                   "destroy the World before unloading its game module (it holds module code pointers)");
    Slot& s = g_slots[module.slot];
    for (u32 i = 0; i < s.type_count; ++i) refl::unregister_type(s.types[i]);
    for (u32 i = 0; i < s.enum_count; ++i) refl::unregister_enum(s.enums[i]);
    platform::unload_library(module.handle);
    release_slot(module.slot);
    module = LoadedModule{};
}

EngineId engine_id() { return MCE_ENGINE_ID; }

u32 loaded_module_count() {
    u32 n = 0;
    for (const Slot& s : g_slots) n += s.used ? 1u : 0u;
    return n;
}

}  // namespace mce
