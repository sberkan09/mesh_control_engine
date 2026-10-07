// Host for the ABI spike: loads the game module at runtime, as game.exe / the editor will,
// and checks that both images agree on type identity and share engine state.
#include <cstdio>

#include "spike_api.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

using namespace mce::spike;

struct Health {  // same name as the module's, different TU: identity is the type name
    mce::i32 hp;
};

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

ModuleEntry load_module(const char* path) {
#if defined(_WIN32)
    HMODULE h = LoadLibraryA(path);
    if (!h) return nullptr;
    return reinterpret_cast<ModuleEntry>(reinterpret_cast<void*>(GetProcAddress(h, "mce_game_module")));
#else
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        std::printf("dlopen: %s\n", dlerror());
        return nullptr;
    }
    return reinterpret_cast<ModuleEntry>(dlsym(h, "mce_game_module"));
#endif
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: spike_host <path to game module>\n");
        return 2;
    }
    const ModuleEntry entry = load_module(argv[1]);
    check(entry != nullptr, "module loads and exports mce_game_module");
    if (!entry) return 1;
    const ModuleInfo* info = entry();

    // Host registers Position first; the module registers Health first. Ids must still agree.
    const mce::u32 host_pos = type_id<Position>();
    const mce::u32 mod_health = info->probe_type_id(1);
    const mce::u32 mod_pos = info->probe_type_id(0);
    const mce::u32 host_health = type_id<Health>();
    check(host_pos == mod_pos, "Position id matches across the DLL edge");
    check(host_health == mod_health, "Health id matches across the DLL edge");
    check(registered_type_count() == 2, "one registry: exactly two types registered");

    EventLog<Damage> log;
    HostContext ctx{&log};
    info->install(ctx);
    check(log.count() == 2, "module pushed into host-owned EventLog via exported instantiation");
    check(log.last().target == 9 && log.last().amount == -5, "event payload intact");

    // Informational: the forbidden pattern. On Windows these differ (one static per image);
    // on ELF with default visibility the dynamic linker happens to unify them. Either way the
    // rule stands, because the result depends on the platform and link flags.
    const mce::u32 host_naive = naive_type_id<Position>();
    const mce::u32 mod_naive = info->probe_naive_id(0);
    std::printf("info naive template-static id: host %u, module %u (%s)\n", host_naive, mod_naive,
                host_naive == mod_naive ? "unified" : "duplicated");

    std::printf("%s\n", g_failures ? "ABI spike FAILED" : "ABI spike passed");
    return g_failures ? 1 : 0;
}
