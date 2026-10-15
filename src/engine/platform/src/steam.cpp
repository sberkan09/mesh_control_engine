// Steam through the Steamworks flat API, opened at run time (M-STEAM step 1, ADR-0042). The entry points
// below are declared here from the flat API's published C signatures; the SDK is not part of the build.

#include "mce/platform/steam.h"

#include <SDL3/SDL.h>

#include <cstdlib>

#include "mce/log.h"
#include "mce/platform/library.h"

namespace mce::platform::steam {

namespace {

// Steamworks' CallbackMsg_t: the pipe's user, the callback's id and its struct.
struct CallbackMsg {
    i32 user;
    i32 callback;
    u8* param;
    i32 param_size;
};

using InitFlatFn = i32 (*)(char* message);  // ESteamAPIInitResult; message is a SteamErrMsg (char[1024])
using InitFn = bool (*)();
using RestartFn = bool (*)(u32 app_id);
using VoidFn = void (*)();
using PipeFn = i32 (*)();
using PipeVoidFn = void (*)(i32 pipe);
using NextCallbackFn = bool (*)(i32 pipe, CallbackMsg* msg);
using U32ObjFn = u32 (*)(void* self);
using BoolObjFn = bool (*)(void* self);
using U64ObjFn = u64 (*)(void* self);
using StrObjFn = const char* (*)(void* self);

struct State {
    void* lib = nullptr;
    bool running = false;
    i32 pipe = 0;
    VoidFn shutdown = nullptr;
    PipeVoidFn run_frame = nullptr;
    NextCallbackFn next = nullptr;
    PipeVoidFn free_last = nullptr;
    void* utils = nullptr;
    void* user = nullptr;
    void* friends = nullptr;
};
State g;

template <typename F>
F find(const char* name) {
    return reinterpret_cast<F>(library_symbol(g.lib, name));
}

void close_library() {
    unload_library(g.lib);
    g = State{};
}

void set_env(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}

// The versioned accessors this build knows, newest first (the flat API keeps old ones exported).
constexpr const char* kUtils[] = {"SteamAPI_SteamUtils_v010", "SteamAPI_SteamUtils_v009"};
constexpr const char* kUser[] = {"SteamAPI_SteamUser_v023", "SteamAPI_SteamUser_v022", "SteamAPI_SteamUser_v021"};
constexpr const char* kFriends[] = {"SteamAPI_SteamFriends_v017"};

}  // namespace

const char* init_result_name(InitResult r) {
    switch (r) {
        case InitResult::Ok: return "ok";
        case InitResult::NoLibrary: return "no_library";
        case InitResult::MissingEntry: return "missing_entry";
        case InitResult::NoSteamClient: return "no_steam_client";
        case InitResult::VersionMismatch: return "version_mismatch";
        case InitResult::Failed: return "failed";
        case InitResult::Restarting: return "restarting";
    }
    return "?";
}

const char* library_file_name() {
#if defined(_WIN32)
    return "steam_api64.dll";
#elif defined(__APPLE__)
    return "libsteam_api.dylib";
#else
    return "libsteam_api.so";
#endif
}

InitResult init(const Config& cfg, FixedString<256>* message) {
    auto say = [&](InitResult r, StringView why) {
        if (message) message->assign(why);
        if (r != InitResult::Ok) MCE_LOG_INFO("steam: {} ({})", init_result_name(r), FixedString<256>(why).c_str());
        return r;
    };
    if (g.running) return say(InitResult::Ok, "already running");
    FixedString<512> path;
    if (!cfg.library_path.empty()) {
        path.assign(cfg.library_path);
    } else {
        const char* base = SDL_GetBasePath();
        path.assign(base ? StringView(base) : StringView());
        path.append(StringView(library_file_name()));
    }
    if (!file_exists(path)) return say(InitResult::NoLibrary, path.view());
    auto lib = load_library(path);
    if (!lib) return say(InitResult::NoLibrary, path.view());
    g.lib = lib.value();

    const auto init_flat = find<InitFlatFn>("SteamAPI_InitFlat");
    const auto init_old = find<InitFn>("SteamAPI_Init");
    const auto restart = find<RestartFn>("SteamAPI_RestartAppIfNecessary");
    const auto dispatch_init = find<VoidFn>("SteamAPI_ManualDispatch_Init");
    const auto get_pipe = find<PipeFn>("SteamAPI_GetHSteamPipe");
    g.shutdown = find<VoidFn>("SteamAPI_Shutdown");
    g.run_frame = find<PipeVoidFn>("SteamAPI_ManualDispatch_RunFrame");
    g.next = find<NextCallbackFn>("SteamAPI_ManualDispatch_GetNextCallback");
    g.free_last = find<PipeVoidFn>("SteamAPI_ManualDispatch_FreeLastCallback");
    if ((!init_flat && !init_old) || !restart || !dispatch_init || !get_pipe || !g.shutdown || !g.run_frame || !g.next ||
        !g.free_last) {
        close_library();
        return say(InitResult::MissingEntry, "steam_api lacks the flat API's init or manual dispatch");
    }

    if (cfg.app_id != 0) {
        if (cfg.restart_if_necessary && restart(cfg.app_id)) {
            close_library();
            return say(InitResult::Restarting, "Steam relaunches the game");
        }
        char id[16];
        SDL_snprintf(id, sizeof(id), "%u", cfg.app_id);
        set_env("SteamAppId", id);
        set_env("SteamGameId", id);
    }

    // Manual dispatch is chosen before init, so no callback ever runs on a thread of Steam's.
    dispatch_init();
    InitResult r = InitResult::Ok;
    char err[1024] = {};
    if (init_flat) {
        switch (init_flat(err)) {
            case 0: break;
            case 2: r = InitResult::NoSteamClient; break;
            case 3: r = InitResult::VersionMismatch; break;
            default: r = InitResult::Failed; break;
        }
    } else if (!init_old()) {
        r = InitResult::NoSteamClient;
    }
    if (r != InitResult::Ok) {
        close_library();
        return say(r, err[0] ? StringView(err) : StringView("SteamAPI_Init refused"));
    }
    g.running = true;
    g.pipe = get_pipe();
    g.utils = interface(Span<const char* const>(kUtils, sizeof(kUtils) / sizeof(kUtils[0])));
    g.user = interface(Span<const char* const>(kUser, sizeof(kUser) / sizeof(kUser[0])));
    g.friends = interface(Span<const char* const>(kFriends, sizeof(kFriends) / sizeof(kFriends[0])));
    MCE_LOG_INFO("steam: running, app {} as {}", app_id(), persona_name());
    return say(InitResult::Ok, "");
}

bool running() { return g.running; }

void shutdown() {
    if (!g.lib) return;
    if (g.running && g.shutdown) g.shutdown();
    close_library();
}

u32 run_callbacks(CallbackFn fn, void* user) {
    if (!g.running) return 0;
    g.run_frame(g.pipe);
    u32 n = 0;
    CallbackMsg msg{};
    while (g.next(g.pipe, &msg)) {
        ++n;
        if (fn) {
            Callback cb;
            cb.id = msg.callback;
            cb.data = Span<const byte>(reinterpret_cast<const byte*>(msg.param), msg.param_size > 0 ? usize(msg.param_size) : 0);
            fn(user, cb);
        }
        g.free_last(g.pipe);
    }
    return n;
}

u32 app_id() {
    const auto f = find<U32ObjFn>("SteamAPI_ISteamUtils_GetAppID");
    return g.running && g.utils && f ? f(g.utils) : 0;
}

u64 steam_id() {
    const auto f = find<U64ObjFn>("SteamAPI_ISteamUser_GetSteamID");
    return g.running && g.user && f ? f(g.user) : 0;
}

const char* persona_name() {
    const auto f = find<StrObjFn>("SteamAPI_ISteamFriends_GetPersonaName");
    const char* name = g.running && g.friends && f ? f(g.friends) : nullptr;
    return name ? name : "";
}

bool on_steam_deck() {
    const auto f = find<BoolObjFn>("SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck");
    return g.running && g.utils && f && f(g.utils);
}

bool big_picture() {
    const auto f = find<BoolObjFn>("SteamAPI_ISteamUtils_IsSteamInBigPictureMode");
    return g.running && g.utils && f && f(g.utils);
}

void* entry(const char* name) { return g.running ? library_symbol(g.lib, name) : nullptr; }

void* interface(Span<const char* const> versioned_names) {
    if (!g.running) return nullptr;
    using AccessorFn = void* (*)();
    for (const char* n : versioned_names)
        if (const auto f = find<AccessorFn>(n)) return f();
    return nullptr;
}

}  // namespace mce::platform::steam
