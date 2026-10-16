#include "mce/input/steam_config.h"

#include <cstdio>

#include "mce/fixed_string.h"

namespace mce::input {
namespace {

using platform::GamepadAxis;
using platform::GamepadButton;

void put(Array<char>& out, StringView s) {
    for (const char c : s) out.push_back(c);
}

// KeyValues text with tab indentation.
struct Kv {
    Array<char>& out;
    u32 depth = 0;
    void indent() {
        for (u32 i = 0; i < depth; ++i) out.push_back('\t');
    }
    void quoted(StringView s) {
        out.push_back('"');
        for (const char c : s) {
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        out.push_back('"');
    }
    void value(StringView key, StringView v) {
        indent();
        quoted(key);
        put(out, "\t\t");
        quoted(v);
        out.push_back('\n');
    }
    void open(StringView key) {
        indent();
        quoted(key);
        out.push_back('\n');
        indent();
        put(out, "{\n");
        ++depth;
    }
    void close() {
        --depth;
        indent();
        put(out, "}\n");
    }
};

enum class SteamKind : u8 { Button, Trigger, Stick };

struct Act {
    StringView name;
    const ActionDef* def = nullptr;
    SteamKind kind = SteamKind::Button;
};

bool bound_to_axis(const InputMapSet& set, const ActionDef& a, GamepadAxis axis) {
    for (u32 b = 0; b < a.binding_count; ++b) {
        const BindingDef& d = set.bindings[a.first_binding + b];
        if (d.device == Device::GamepadAxis && static_cast<GamepadAxis>(d.code) == axis) return true;
    }
    return false;
}

// Every labeled action once (the first map that has it), in map order.
void collect(const InputMapSet& set, Array<Act>& out) {
    out.clear();
    for (usize i = 0; i < set.actions.size(); ++i) {
        const StringView label = set.label(i);
        if (label.empty()) continue;
        bool seen = false;
        for (const Act& a : out) seen = seen || a.def->name == set.actions[i].name;
        if (seen) continue;
        Act a;
        a.name = label;
        a.def = &set.actions[i];
        if (a.def->type == ActionType::Button)
            a.kind = SteamKind::Button;
        else if (a.def->type == ActionType::Axis1D && (bound_to_axis(set, *a.def, GamepadAxis::LeftTrigger) ||
                                                      bound_to_axis(set, *a.def, GamepadAxis::RightTrigger)))
            a.kind = SteamKind::Trigger;
        else
            a.kind = SteamKind::Stick;
        out.push_back(a);
    }
}

void token(FixedString<96>& t, const char* prefix, StringView name) {
    t.assign(prefix);
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        const char s[2] = {ok ? c : '_', 0};
        t.append(StringView(s, 1));
    }
}

// The labeled Button action bound to `button`, or none.
const Act* button_action(const InputMapSet& set, Span<const Act> acts, GamepadButton button) {
    for (const Act& a : acts) {
        if (a.kind != SteamKind::Button) continue;
        for (u32 b = 0; b < a.def->binding_count; ++b) {
            const BindingDef& d = set.bindings[a.def->first_binding + b];
            if (d.device == Device::GamepadButton && static_cast<GamepadButton>(d.code) == button) return &a;
        }
    }
    return nullptr;
}

// The labeled action reading `axis` (a stick or a trigger as an axis), or a Button pulled by it.
const Act* axis_action(const InputMapSet& set, Span<const Act> acts, GamepadAxis axis, bool button) {
    for (const Act& a : acts) {
        if ((a.kind == SteamKind::Button) != button) continue;
        if (bound_to_axis(set, *a.def, axis)) return &a;
    }
    return nullptr;
}

void binding(Kv& kv, StringView input, const Act* a) {
    if (!a) return;
    kv.open(input);
    kv.open("activators");
    kv.open("Full_Press");
    kv.open("bindings");
    FixedString<160> b("game_action ");
    b.append(StringView(kSteamActionSet));
    b.append(" ");
    b.append(a->name);
    b.append(", , ");
    kv.value("binding", b.view());
    kv.close();
    kv.close();
    kv.close();
    kv.close();
}

void number(FixedString<16>& s, u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", v);
    s.assign(buf);
}

}  // namespace

void steam_manifest_file(u32 app_id, Array<char>& out) {
    out.clear();
    char buf[48];
    std::snprintf(buf, sizeof(buf), "game_actions_%u.vdf", app_id);
    put(out, buf);
}

const char* steam_controller_type(SteamController c) {
    switch (c) {
        case SteamController::Deck: return "controller_neptune";
        case SteamController::Xbox: return "controller_xboxone";
        case SteamController::PlayStation: return "controller_ps4";
        case SteamController::Switch: return "controller_switch_pro";
    }
    return "controller_xboxone";
}

const char* steam_controller_file(SteamController c) {
    switch (c) {
        case SteamController::Deck: return "controller_neptune.vdf";
        case SteamController::Xbox: return "controller_xboxone.vdf";
        case SteamController::PlayStation: return "controller_ps4.vdf";
        case SteamController::Switch: return "controller_switch_pro.vdf";
    }
    return "controller_xboxone.vdf";
}

void steam_action_manifest(const InputMapSet& set, StringView title, Array<char>& out) {
    out.clear();
    Array<Act> acts;
    collect(set, acts);
    Kv kv{out};
    kv.open("In Game Actions");
    kv.open("actions");
    kv.open(kSteamActionSet);
    kv.value("title", "#Set_Game");
    FixedString<96> t;
    kv.open("StickPadGyro");
    for (const Act& a : acts) {
        if (a.kind != SteamKind::Stick) continue;
        kv.open(a.name);
        token(t, "#Action_", a.name);
        kv.value("title", t.view());
        kv.value("input_mode", "joystick_move");
        kv.close();
    }
    kv.close();
    kv.open("AnalogTrigger");
    for (const Act& a : acts) {
        if (a.kind != SteamKind::Trigger) continue;
        token(t, "#Action_", a.name);
        kv.value(a.name, t.view());
    }
    kv.close();
    kv.open("Button");
    for (const Act& a : acts) {
        if (a.kind != SteamKind::Button) continue;
        token(t, "#Action_", a.name);
        kv.value(a.name, t.view());
    }
    kv.close();
    kv.close();
    kv.close();
    kv.open("localization");
    kv.open("english");
    kv.value("Title", title);
    kv.value("Set_Game", "Game");
    for (const Act& a : acts) {
        token(t, "Action_", a.name);
        kv.value(t.view(), a.name);
    }
    kv.close();
    kv.close();
    kv.open("configurations");
    for (u32 c = 0; c < kSteamControllerCount; ++c) {
        const auto ct = static_cast<SteamController>(c);
        kv.open(steam_controller_type(ct));
        kv.open("0");
        kv.value("path", steam_controller_file(ct));
        kv.close();
        kv.close();
    }
    kv.close();
    kv.close();
}

void steam_controller_config(const InputMapSet& set, SteamController c, StringView title, Array<char>& out) {
    out.clear();
    Array<Act> acts;
    collect(set, acts);
    const Span<const Act> a = acts.span();
    Kv kv{out};
    kv.open("controller_mappings");
    kv.value("version", "3");
    kv.value("revision", "1");
    kv.value("title", "#Title");
    kv.value("description", "#Description");
    kv.value("creator", "0");
    kv.value("controller_type", steam_controller_type(c));
    kv.open("actions");
    kv.open(kSteamActionSet);
    kv.value("title", "#Set_Game");
    kv.value("legacy_set", "0");
    kv.close();
    kv.close();
    kv.open("localization");
    kv.open("english");
    kv.value("Title", title);
    kv.value("Description", "Generated from the game's input map");
    kv.value("Set_Game", "Game");
    kv.close();
    kv.close();

    // Groups, each with its source in the preset.
    struct Source {
        u32 id;
        const char* source;
    };
    Source sources[12];
    u32 n = 0;
    auto group = [&](const char* mode, const char* source) {
        FixedString<16> id;
        number(id, n);
        kv.open("group");
        kv.value("id", id.view());
        kv.value("mode", mode);
        sources[n] = Source{n, source};
        ++n;
    };
    auto analog = [&](const Act* act) {
        if (!act) return;
        kv.open("gameactions");
        kv.value(kSteamActionSet, act->name);
        kv.close();
    };

    group("four_buttons", "button_diamond");
    kv.open("inputs");
    binding(kv, "button_a", button_action(set, a, GamepadButton::South));
    binding(kv, "button_b", button_action(set, a, GamepadButton::East));
    binding(kv, "button_x", button_action(set, a, GamepadButton::West));
    binding(kv, "button_y", button_action(set, a, GamepadButton::North));
    kv.close();
    kv.close();

    group("dpad", "dpad");
    kv.open("inputs");
    binding(kv, "dpad_north", button_action(set, a, GamepadButton::DpadUp));
    binding(kv, "dpad_south", button_action(set, a, GamepadButton::DpadDown));
    binding(kv, "dpad_east", button_action(set, a, GamepadButton::DpadRight));
    binding(kv, "dpad_west", button_action(set, a, GamepadButton::DpadLeft));
    kv.close();
    kv.close();

    const Act* left = axis_action(set, a, GamepadAxis::LeftX, false);
    if (!left) left = axis_action(set, a, GamepadAxis::LeftY, false);
    group("joystick_move", "joystick");
    analog(left);
    kv.open("inputs");
    binding(kv, "click", button_action(set, a, GamepadButton::LeftStick));
    kv.close();
    kv.close();

    const Act* right = axis_action(set, a, GamepadAxis::RightX, false);
    if (!right) right = axis_action(set, a, GamepadAxis::RightY, false);
    group("joystick_move", "right_joystick");
    analog(right);
    kv.open("inputs");
    binding(kv, "click", button_action(set, a, GamepadButton::RightStick));
    kv.close();
    kv.close();

    for (const GamepadAxis t : {GamepadAxis::LeftTrigger, GamepadAxis::RightTrigger}) {
        group("trigger", t == GamepadAxis::LeftTrigger ? "left_trigger" : "right_trigger");
        analog(axis_action(set, a, t, false));
        kv.open("inputs");
        binding(kv, "click", axis_action(set, a, t, true));
        kv.close();
        kv.close();
    }

    group("switches", "switch");
    kv.open("inputs");
    binding(kv, "button_escape", button_action(set, a, GamepadButton::Back));
    binding(kv, "button_menu", button_action(set, a, GamepadButton::Start));
    binding(kv, "left_bumper", button_action(set, a, GamepadButton::LeftShoulder));
    binding(kv, "right_bumper", button_action(set, a, GamepadButton::RightShoulder));
    binding(kv, "button_back_left", button_action(set, a, GamepadButton::LeftPaddle1));
    binding(kv, "button_back_right", button_action(set, a, GamepadButton::RightPaddle1));
    binding(kv, "button_back_left_upper", button_action(set, a, GamepadButton::LeftPaddle2));
    binding(kv, "button_back_right_upper", button_action(set, a, GamepadButton::RightPaddle2));
    kv.close();
    kv.close();

    if (c == SteamController::Deck && right) {  // the Deck's right trackpad aims like its right stick
        group("joystick_move", "right_trackpad");
        analog(right);
        kv.close();
    }

    kv.open("preset");
    kv.value("id", "0");
    kv.value("name", kSteamActionSet);
    kv.open("group_source_bindings");
    for (u32 i = 0; i < n; ++i) {
        FixedString<16> id;
        number(id, sources[i].id);
        FixedString<48> v(sources[i].source);
        v.append(" active");
        kv.value(id.view(), v.view());
    }
    kv.close();
    kv.close();
    kv.close();
}

}  // namespace mce::input
