// Gamepads (v0.5, FR-INP-010): hot-plug into player slots, the standard-layout event stream, rumble,
// and SDL virtual pads for tests and bots. SDL_Gamepad never leaves this file.

#include <SDL3/SDL.h>

#include "internal.h"
#include "mce/log.h"
#include "mce/math/math.h"
#include "mce/platform/platform.h"

namespace mce::platform {

static_assert(static_cast<u32>(GamepadButton::Count) == SDL_GAMEPAD_BUTTON_COUNT, "GamepadButton mirrors SDL");
static_assert(static_cast<u32>(GamepadAxis::Count) == SDL_GAMEPAD_AXIS_COUNT, "GamepadAxis mirrors SDL");

namespace {

struct GamepadSlot {
    SDL_Gamepad* pad = nullptr;
    SDL_JoystickID id = 0;
};
GamepadSlot g_pads[kMaxGamepads];

constexpr u32 kMaxVirtualPads = 8;
struct VirtualPad {
    SDL_JoystickID id = 0;
    SDL_Joystick* joy = nullptr;
    bool reserved = false;
    VirtualRumble rumble;  // the motors as the last rumble set them
};
VirtualPad g_virtual[kMaxVirtualPads];

// SDL's rumble callback for a virtual pad: SDL_RumbleGamepad starts it, and SDL sets 0, 0 when the
// duration runs out.
bool SDLCALL virtual_rumble(void* user, Uint16 low, Uint16 high) {
    auto* v = static_cast<VirtualPad*>(user);
    v->rumble.low = static_cast<f32>(low) / 65535.f;
    v->rumble.high = static_cast<f32>(high) / 65535.f;
    if (low || high) ++v->rumble.count;
    return true;
}

}  // namespace

namespace detail {

u32 find_gamepad(u32 instance_id) {
    for (u32 s = 0; s < kMaxGamepads; ++s)
        if (g_pads[s].pad && g_pads[s].id == instance_id) return s;
    return kNoSlot;
}

u32 open_gamepad(u32 instance_id) {
    if (find_gamepad(instance_id) != kNoSlot) return kNoSlot;  // already open: no second Added
    for (u32 s = 0; s < kMaxGamepads; ++s) {
        if (g_pads[s].pad) continue;
        SDL_Gamepad* pad = SDL_OpenGamepad(instance_id);
        if (!pad) {
            MCE_LOG_WARN("platform: cannot open gamepad: {}", SDL_GetError());
            return kNoSlot;
        }
        g_pads[s] = {pad, instance_id};
        SDL_SetGamepadPlayerIndex(pad, static_cast<int>(s));
        const char* name = SDL_GetGamepadName(pad);
        MCE_LOG_INFO("platform: gamepad '{}' in slot {}", name ? name : "?", s);
        return s;
    }
    MCE_LOG_WARN("platform: more than {} gamepads; ignoring the new one", kMaxGamepads);
    return kNoSlot;
}

u32 close_gamepad(u32 instance_id) {
    const u32 s = find_gamepad(instance_id);
    if (s == kNoSlot) return kNoSlot;
    SDL_CloseGamepad(g_pads[s].pad);
    g_pads[s] = GamepadSlot{};
    return s;
}

void close_gamepads() {
    for (VirtualPad& v : g_virtual)
        if (v.joy) {
            SDL_CloseJoystick(v.joy);
            SDL_DetachVirtualJoystick(v.id);
            v = VirtualPad{};
        }
    for (GamepadSlot& p : g_pads) {
        if (p.pad) SDL_CloseGamepad(p.pad);
        p = GamepadSlot{};
    }
}

f32 gamepad_axis_value(u8 axis, i16 raw) {
    // -32768..32767 -> -1..1 (triggers report 0..32767 -> 0..1).
    const f32 v = static_cast<f32>(raw) / 32767.f;
    (void)axis;
    return math::clamp(v, -1.f, 1.f);
}

}  // namespace detail

bool gamepad_connected(u32 slot) { return slot < kMaxGamepads && g_pads[slot].pad != nullptr; }

const char* gamepad_name(u32 slot) {
    if (!gamepad_connected(slot)) return "";
    const char* n = SDL_GetGamepadName(g_pads[slot].pad);
    return n ? n : "";
}

bool gamepad_rumble(u32 slot, f32 low, f32 high, u32 ms) {
    if (!gamepad_connected(slot)) return false;
    auto to16 = [](f32 v) { return static_cast<Uint16>(math::clamp(v, 0.f, 1.f) * 65535.f); };
    return SDL_RumbleGamepad(g_pads[slot].pad, to16(low), to16(high), ms);
}

u32 attach_virtual_gamepad(const char* name) {
    if (!is_initialized()) return 0;
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
    d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
    d.name = name;
    VirtualPad* slot = nullptr;
    for (VirtualPad& v : g_virtual)
        if (!v.reserved) {
            slot = &v;
            break;
        }
    if (!slot) {
        MCE_LOG_ERROR("platform: virtual gamepad: all {} in use", kMaxVirtualPads);
        return 0;
    }
    *slot = VirtualPad{};
    slot->reserved = true;
    d.userdata = slot;
    d.Rumble = &virtual_rumble;
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    if (id == 0) {
        MCE_LOG_ERROR("platform: virtual gamepad: {}", SDL_GetError());
        slot->reserved = false;
        return 0;
    }
    // Open the joystick side once so the setters have a device to write into.
    slot->id = id;
    slot->joy = SDL_OpenJoystick(id);
    return id;
}

void detach_virtual_gamepad(u32 id) {
    for (VirtualPad& v : g_virtual)
        if (v.id == id && v.reserved) {
            if (v.joy) SDL_CloseJoystick(v.joy);
            SDL_DetachVirtualJoystick(id);
            v = VirtualPad{};
            return;
        }
    SDL_DetachVirtualJoystick(id);
}

VirtualRumble virtual_gamepad_rumble(u32 id) {
    for (const VirtualPad& v : g_virtual)
        if (v.id == id && v.reserved) return v.rumble;
    return {};
}

void set_virtual_gamepad_button(u32 id, GamepadButton b, bool down) {
    for (VirtualPad& v : g_virtual)
        if (v.id == id && v.joy) SDL_SetJoystickVirtualButton(v.joy, static_cast<int>(b), down);
}

void set_virtual_gamepad_axis(u32 id, GamepadAxis a, f32 value) {
    // The virtual pad is a joystick under SDL's automatic gamepad mapping, which reads a trigger's
    // full joystick range (-32768..32767) as 0..1.
    const bool trigger = a == GamepadAxis::LeftTrigger || a == GamepadAxis::RightTrigger;
    const f32 raw = trigger ? math::clamp(value, 0.f, 1.f) * 65535.f - 32768.f : math::clamp(value, -1.f, 1.f) * 32767.f;
    for (VirtualPad& v : g_virtual)
        if (v.id == id && v.joy) SDL_SetJoystickVirtualAxis(v.joy, static_cast<int>(a), static_cast<Sint16>(raw));
}

}  // namespace mce::platform
