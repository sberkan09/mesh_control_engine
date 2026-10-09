#include "mce/module_host.h"

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "mce/log.h"
#include "mce/platform/library.h"

namespace mce {
namespace {

// Copy-and-load slots. Recompile-between-Plays needs 2 per module; tests load a few at once.
constexpr u32 kSlotCount = 8;

struct Slot {
    bool used = false;
    u32 worlds_alive = 0;  // Worlds the module was installed into that still exist
    FixedString<512> loaded_path;
};
Slot g_slots[kSlotCount];

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
    std::remove(s.loaded_path.c_str());
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

    // Lowest copy index not mapped right now: a rebuilt module never lands on a mapped image.
    u32 n = 0;
    while (copy_in_use(copy_path(path, n).view())) ++n;
    LoadedModule m;
    m.slot = slot;
    m.source_path = path;
    m.loaded_path = copy_path(path, n);
    if (m.source_path.truncated() || m.loaded_path.truncated())
        return Error(Errc::InvalidArgument, "module.copy", path);

    if (const ResultV copied = platform::copy_file(path, m.loaded_path); !copied)
        return Error(Errc::IoError, "module.copy", m.loaded_path);
    g_slots[slot].used = true;
    g_slots[slot].loaded_path = m.loaded_path;

    const Result<void*> lib = platform::load_library(m.loaded_path);
    if (!lib) {
        release_slot(slot);
        return Error(Errc::ModuleNotFound, "module.open", path);
    }
    m.handle = lib.value();

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
