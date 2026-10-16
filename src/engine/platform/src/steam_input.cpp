// Steam Input over the flat API (M-STEAM step 4; mce/platform/steam_input.h). Steam Input is
// pumped by the game (Init with explicit RunFrame), the action set of the input map on top is
// activated on every controller, and each action is read by handle. Handles are looked up by
// name and kept once Steam knows them (it knows none before the action manifest is loaded).
//
// The data structs are the flat API's own, packed to 1 byte as Steam declares them; they come back
// by value.

#include "mce/platform/steam_input.h"

#include <cmath>

#include "mce/log.h"
#include "mce/platform/steam.h"

namespace mce::platform::steam::input {
namespace {

#pragma pack(push, 1)
struct DigitalData {  // InputDigitalActionData_t
    bool state;
    bool active;
};
struct AnalogData {  // InputAnalogActionData_t
    i32 mode;
    f32 x;
    f32 y;
    bool active;
};
#pragma pack(pop)

constexpr u64 kAllControllers = ~u64(0);  // STEAM_INPUT_HANDLE_ALL_CONTROLLERS
constexpr u32 kMaxControllers = 16;       // STEAM_INPUT_MAX_COUNT
constexpr u32 kMaxOrigins = 8;            // STEAM_INPUT_MAX_ORIGINS
constexpr i32 kGlyphSmall = 0;            // k_ESteamInputGlyphSize_Small

using InitFn = bool (*)(void* self, bool explicit_frame);
using ShutdownFn = bool (*)(void* self);
using RunFrameFn = void (*)(void* self, bool reserved);
using ControllersFn = i32 (*)(void* self, u64* out);
using HandleFn = u64 (*)(void* self, const char* name);
using ActivateFn = void (*)(void* self, u64 controller, u64 set);
using DigitalFn = DigitalData (*)(void* self, u64 controller, u64 action);
using AnalogFn = AnalogData (*)(void* self, u64 controller, u64 action);
using OriginsFn = i32 (*)(void* self, u64 controller, u64 set, u64 action, i32* out);
using OriginStrFn = const char* (*)(void* self, i32 origin);
using GlyphFn = const char* (*)(void* self, i32 origin, i32 size, u32 flags);

constexpr const char* kInput[] = {"SteamAPI_SteamInput_v006", "SteamAPI_SteamInput_v005", "SteamAPI_SteamInput_v001"};

template <typename F>
F fn(const char* name) {
    return reinterpret_cast<F>(entry(name));
}

struct Handle {
    FixedString<64> name;
    u8 kind = 0;  // 0 set, 1 digital, 2 analog
    u64 handle = 0;
};

struct State {
    void* self = nullptr;
    bool on = false;
    u64 pads[kMaxControllers] = {};
    u32 pad_count = 0;
    Handle handles[192];
    u32 handle_count = 0;
};
State g;

// The handle of a set (kind 0) or an action (1 digital, 2 analog); 0 while Steam does not know it.
u64 handle(StringView name, u8 kind) {
    for (u32 i = 0; i < g.handle_count; ++i)
        if (g.handles[i].kind == kind && g.handles[i].name.view() == name) return g.handles[i].handle;
    const char* getter = kind == 0   ? "SteamAPI_ISteamInput_GetActionSetHandle"
                         : kind == 1 ? "SteamAPI_ISteamInput_GetDigitalActionHandle"
                                     : "SteamAPI_ISteamInput_GetAnalogActionHandle";
    const auto get = fn<HandleFn>(getter);
    const FixedString<64> z(name);
    const u64 h = get ? get(g.self, z.c_str()) : 0;
    if (h != 0 && g.handle_count < sizeof(g.handles) / sizeof(g.handles[0])) g.handles[g.handle_count++] = Handle{z, kind, h};
    return h;
}

}  // namespace

bool start() {
    if (g.on) return true;
    g = State{};
    if (!steam::running()) return false;
    g.self = interface(Span<const char* const>(kInput, sizeof(kInput) / sizeof(kInput[0])));
    const auto init = fn<InitFn>("SteamAPI_ISteamInput_Init");
    if (!g.self || !init || !fn<RunFrameFn>("SteamAPI_ISteamInput_RunFrame") || !init(g.self, true)) {
        g = State{};
        MCE_LOG_INFO("steam: no Steam Input in this steam_api");
        return false;
    }
    g.on = true;
    return true;
}

bool running() { return g.on && steam::running(); }

void stop() {
    if (g.on && steam::running())
        if (const auto shutdown = fn<ShutdownFn>("SteamAPI_ISteamInput_Shutdown")) shutdown(g.self);
    g = State{};
}

u32 controllers() { return running() ? g.pad_count : 0; }

u32 frame(const char* set, Span<const Action> actions, Span<Value> out) {
    for (usize i = 0; i < actions.size() && i < out.size(); ++i) out[i] = Value{};
    if (!running()) return 0;
    fn<RunFrameFn>("SteamAPI_ISteamInput_RunFrame")(g.self, false);
    const auto connected = fn<ControllersFn>("SteamAPI_ISteamInput_GetConnectedControllers");
    const i32 n = connected ? connected(g.self, g.pads) : 0;
    g.pad_count = n > 0 ? u32(n < i32(kMaxControllers) ? n : i32(kMaxControllers)) : 0;
    if (g.pad_count == 0) return 0;
    if (const u64 s = handle(StringView(set), 0))
        if (const auto activate = fn<ActivateFn>("SteamAPI_ISteamInput_ActivateActionSet")) activate(g.self, kAllControllers, s);
    const auto digital = fn<DigitalFn>("SteamAPI_ISteamInput_GetDigitalActionData");
    const auto analog = fn<AnalogFn>("SteamAPI_ISteamInput_GetAnalogActionData");
    for (usize i = 0; i < actions.size() && i < out.size(); ++i) {
        const Action& a = actions[i];
        Value& v = out[i];
        const u64 h = handle(a.name.view(), a.kind == Kind::Digital ? 1 : 2);
        if (h == 0) continue;
        for (u32 p = 0; p < g.pad_count; ++p) {
            if (a.kind == Kind::Digital) {
                if (!digital) break;
                const DigitalData d = digital(g.self, g.pads[p], h);
                v.active = v.active || d.active;
                if (d.active && d.state) v.x = 1.f;
            } else {
                if (!analog) break;
                const AnalogData d = analog(g.self, g.pads[p], h);
                v.active = v.active || d.active;
                if (d.active && d.x * d.x + d.y * d.y > v.x * v.x + v.y * v.y) {
                    v.x = d.x;
                    v.y = d.y;
                }
            }
        }
    }
    return g.pad_count;
}

bool origin(const char* set, const Action& action, FixedString<64>& name, FixedString<256>& glyph) {
    name.clear();
    glyph.clear();
    if (!running() || g.pad_count == 0) return false;
    const u64 s = handle(StringView(set), 0);
    const u64 h = handle(action.name.view(), action.kind == Kind::Digital ? 1 : 2);
    const auto origins = fn<OriginsFn>(action.kind == Kind::Digital ? "SteamAPI_ISteamInput_GetDigitalActionOrigins"
                                                                   : "SteamAPI_ISteamInput_GetAnalogActionOrigins");
    if (s == 0 || h == 0 || !origins) return false;
    i32 found[kMaxOrigins] = {};
    if (origins(g.self, g.pads[0], s, h, found) <= 0) return false;
    if (const auto str = fn<OriginStrFn>("SteamAPI_ISteamInput_GetStringForActionOrigin"))
        if (const char* t = str(g.self, found[0])) name.assign(t);
    if (const auto png = fn<GlyphFn>("SteamAPI_ISteamInput_GetGlyphPNGForActionOrigin"))
        if (const char* p = png(g.self, found[0], kGlyphSmall, 0)) glyph.assign(p);
    return !name.empty();
}

}  // namespace mce::platform::steam::input
