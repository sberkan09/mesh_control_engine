// .mcinput -> CHNK_INPUT: a project's input action maps (FR-INP-020, assets.md §2). The chunk is
// the one InputSystem::load_map reads (mce/input/input_map.h), written by encode_input_map and
// validated by decode_input_map before it is stored, so a map the runtime would reject never cooks.
//
// {
//   "format": "mce-input", "version": 1, "guid": "<uuid>",
//   "maps": [
//     { "name": "Gameplay", "blocks_below": false,
//       "actions": [
//         { "name": "Move", "type": "axis2d",       // button (default) | axis1d | axis2d
//           "bindings": [
//             { "key": "D", "axis": "x", "scale": 1 },              // axis: x (default) | y
//             { "gamepad_axis": "LeftY", "axis": "y", "scale": -1, "deadzone": [0.2, 1], "curve": "linear" },
//             { "gamepad_button": "South" }, { "mouse_button": "Left" },
//             { "mouse_delta": "x" }, { "mouse_wheel": "y" }, { "key": "Space", "invert": true },
//             { "touch_stick": "x", "zone": [0, 0, 0.5, 1] }, { "touch_stick": "y", "scale": -1, "zone": [0, 0, 0.5, 1] },
//             { "touch_zone": [0.8, 0.7, 1, 1] }, { "touch_tap": [0.5, 0, 1, 1] }, { "touch_swipe": "x" }
//           ] } ] } ]
// }
//
// Touch (M-MOBILE): zones are [x0, y0, x1, y1] over the window, 0..1, y down. touch_zone and
// touch_tap name their zone; touch_stick and touch_swipe name the source axis (like mouse_delta)
// and take an optional "zone" (default: the whole window).
//
// Exactly one device key per binding. Key names are the platform::Key enumerators ("A".."Z",
// "Num0".."Num9" or "0".."9", "Space", "Return", "Escape", "Left", "LShift", "F1", ...); gamepad
// names are the SDL standard-layout ones in platform::GamepadButton / GamepadAxis. The first map
// is the one the stack starts with.

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/input/input_map.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.input";
constexpr u32 kInputFormatVersion = 1;

struct Named {
    const char* name;
    u32 code;
};

using platform::GamepadAxis;
using platform::GamepadButton;
using platform::Key;
using platform::MouseButton;

constexpr u32 code(Key k) { return static_cast<u32>(k); }
constexpr u32 code(GamepadButton b) { return static_cast<u32>(b); }
constexpr u32 code(GamepadAxis a) { return static_cast<u32>(a); }
constexpr u32 code(MouseButton b) { return static_cast<u32>(b); }

constexpr Named kKeys[] = {
    {"Return", code(Key::Return)}, {"Enter", code(Key::Return)}, {"Escape", code(Key::Escape)},
    {"Backspace", code(Key::Backspace)}, {"Tab", code(Key::Tab)}, {"Space", code(Key::Space)},
    {"Grave", code(Key::Grave)}, {"Right", code(Key::Right)}, {"Left", code(Key::Left)},
    {"Down", code(Key::Down)}, {"Up", code(Key::Up)}, {"LCtrl", code(Key::LCtrl)},
    {"LShift", code(Key::LShift)}, {"LAlt", code(Key::LAlt)}, {"LGui", code(Key::LGui)},
    {"RCtrl", code(Key::RCtrl)}, {"RShift", code(Key::RShift)}, {"RAlt", code(Key::RAlt)},
    {"RGui", code(Key::RGui)},
};

constexpr Named kGamepadButtons[] = {
    {"South", code(GamepadButton::South)}, {"East", code(GamepadButton::East)},
    {"West", code(GamepadButton::West)}, {"North", code(GamepadButton::North)},
    {"Back", code(GamepadButton::Back)}, {"Guide", code(GamepadButton::Guide)},
    {"Start", code(GamepadButton::Start)}, {"LeftStick", code(GamepadButton::LeftStick)},
    {"RightStick", code(GamepadButton::RightStick)}, {"LeftShoulder", code(GamepadButton::LeftShoulder)},
    {"RightShoulder", code(GamepadButton::RightShoulder)}, {"DpadUp", code(GamepadButton::DpadUp)},
    {"DpadDown", code(GamepadButton::DpadDown)}, {"DpadLeft", code(GamepadButton::DpadLeft)},
    {"DpadRight", code(GamepadButton::DpadRight)}, {"Misc1", code(GamepadButton::Misc1)},
    {"RightPaddle1", code(GamepadButton::RightPaddle1)}, {"LeftPaddle1", code(GamepadButton::LeftPaddle1)},
    {"RightPaddle2", code(GamepadButton::RightPaddle2)}, {"LeftPaddle2", code(GamepadButton::LeftPaddle2)},
    {"Touchpad", code(GamepadButton::Touchpad)},
};

constexpr Named kGamepadAxes[] = {
    {"LeftX", code(GamepadAxis::LeftX)}, {"LeftY", code(GamepadAxis::LeftY)},
    {"RightX", code(GamepadAxis::RightX)}, {"RightY", code(GamepadAxis::RightY)},
    {"LeftTrigger", code(GamepadAxis::LeftTrigger)}, {"RightTrigger", code(GamepadAxis::RightTrigger)},
};

constexpr Named kMouseButtons[] = {
    {"Left", code(MouseButton::Left)}, {"Middle", code(MouseButton::Middle)}, {"Right", code(MouseButton::Right)},
    {"X1", code(MouseButton::X1)}, {"X2", code(MouseButton::X2)},
};

template <usize N>
bool find_named(const Named (&table)[N], StringView name, u32& out) {
    for (const Named& n : table) {
        if (name == StringView(n.name)) {
            out = n.code;
            return true;
        }
    }
    return false;
}

// "A".."Z", "0".."9", "Num0".."Num9", "F1".."F12", then the named keys.
bool find_key(StringView name, u32& out) {
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') {
        out = code(Key::A) + static_cast<u32>(name[0] - 'A');
        return true;
    }
    StringView digit = name;
    if (digit.size() == 4 && digit.substr(0, 3) == "Num") digit = digit.substr(3);
    if (digit.size() == 1 && digit[0] >= '0' && digit[0] <= '9') {
        out = digit[0] == '0' ? code(Key::Num0) : code(Key::Num1) + static_cast<u32>(digit[0] - '1');
        return true;
    }
    if (name.size() >= 2 && name.size() <= 3 && name[0] == 'F') {
        u32 n = 0;
        for (usize i = 1; i < name.size(); ++i) {
            if (name[i] < '0' || name[i] > '9') return find_named(kKeys, name, out);
            n = n * 10 + static_cast<u32>(name[i] - '0');
        }
        if (n >= 1 && n <= 12) {
            out = code(Key::F1) + n - 1;
            return true;
        }
    }
    return find_named(kKeys, name, out);
}

void error(DiagList& diags, StringView file, JsonValue at, StringView msg) {
    detail::error_at(diags, Errc::ParseError, file, at, kStage, msg);
}

// "x" -> 0, "y" -> 1; anything else is an error.
// [x0, y0, x1, y1], each in 0..1 with x0 < x1 and y0 < y1.
bool read_zone(JsonValue v, u32& out, StringView file, DiagList& diags) {
    bool ok = v.is_array() && v.size() == 4;
    f32 c[4] = {};
    for (u32 i = 0; ok && i < 4; ++i) {
        ok = v.at(i).is_number() && v.at(i).as_number() >= 0.0 && v.at(i).as_number() <= 1.0;
        if (ok) c[i] = static_cast<f32>(v.at(i).as_number());
    }
    if (!ok || !(c[0] < c[2]) || !(c[1] < c[3])) {
        error(diags, file, v, "a touch zone must be [x0, y0, x1, y1] in 0..1 with x0 < x1 and y0 < y1");
        return false;
    }
    out = input::pack_touch_zone({c[0], c[1], c[2], c[3]});
    return true;
}

bool read_axis(JsonValue v, u8& out, StringView file, DiagList& diags) {
    const StringView s = v.is_string() ? v.as_string() : StringView();
    if (s == "x") out = 0;
    else if (s == "y") out = 1;
    else {
        error(diags, file, v, "expected \"x\" or \"y\"");
        return false;
    }
    return true;
}

void read_binding(JsonValue b, input::ActionType type, input::InputMapSet& set, StringView file, DiagList& diags) {
    if (!b.is_object()) {
        error(diags, file, b, "a binding must be an object");
        return;
    }
    if (!detail::check_keys(b, {"key", "mouse_button", "mouse_delta", "mouse_wheel", "gamepad_button", "gamepad_axis",
                                "touch_zone", "touch_tap", "touch_stick", "touch_swipe", "zone", "axis", "scale",
                                "deadzone", "curve", "invert"},
                            file, kStage, diags))
        return;
    input::BindingDef def;
    u32 devices = 0;
    const auto named = [&](StringView key, input::Device device, auto&& lookup, const char* what) {
        const JsonValue v = b.get(key);
        if (!v.valid()) return;
        ++devices;
        def.device = device;
        if (!v.is_string() || !lookup(v.as_string(), def.code)) {
            String msg("unknown ");
            msg += what;
            if (v.is_string()) {
                msg += " '";
                msg += v.as_string();
                msg += "'";
            }
            error(diags, file, v, msg);
        }
    };
    named("key", input::Device::Keyboard, find_key, "key");
    named("mouse_button", input::Device::MouseButton, [](StringView s, u32& o) { return find_named(kMouseButtons, s, o); },
          "mouse button");
    named("gamepad_button", input::Device::GamepadButton, [](StringView s, u32& o) { return find_named(kGamepadButtons, s, o); },
          "gamepad button");
    named("gamepad_axis", input::Device::GamepadAxis, [](StringView s, u32& o) { return find_named(kGamepadAxes, s, o); },
          "gamepad axis");
    // Mouse motion and wheel name the source axis; it also drives that action component unless
    // "axis" says otherwise.
    for (const auto& [key, device] : {std::pair<StringView, input::Device>{"mouse_delta", input::Device::MouseDelta},
                                      std::pair<StringView, input::Device>{"mouse_wheel", input::Device::MouseWheel}}) {
        const JsonValue v = b.get(key);
        if (!v.valid()) continue;
        ++devices;
        def.device = device;
        u8 src = 0;
        if (read_axis(v, src, file, diags)) {
            def.code = src;
            def.axis = src;
        }
    }
    for (const auto& [key, device] : {std::pair<StringView, input::Device>{"touch_zone", input::Device::TouchZone},
                                      std::pair<StringView, input::Device>{"touch_tap", input::Device::TouchTap}}) {
        const JsonValue v = b.get(key);
        if (!v.valid()) continue;
        ++devices;
        def.device = device;
        read_zone(v, def.code, file, diags);
    }
    bool zoned_axis = false;
    for (const auto& [key, device] : {std::pair<StringView, input::Device>{"touch_stick", input::Device::TouchStick},
                                      std::pair<StringView, input::Device>{"touch_swipe", input::Device::TouchSwipe}}) {
        const JsonValue v = b.get(key);
        if (!v.valid()) continue;
        ++devices;
        zoned_axis = true;
        def.device = device;
        def.code = input::pack_touch_zone({});
        u8 src = 0;
        if (read_axis(v, src, file, diags)) {
            def.axis = src;
            if (src == 1) def.flags |= input::BindingFlag::SourceY;
        }
        if (const JsonValue z = b.get("zone"); z.valid()) read_zone(z, def.code, file, diags);
    }
    if (devices != 1) {
        error(diags, file, b,
              "a binding names exactly one of key, mouse_button, mouse_delta, mouse_wheel, gamepad_button, gamepad_axis, "
              "touch_zone, touch_tap, touch_stick, touch_swipe");
        return;
    }
    if (!zoned_axis && b.get("zone").valid()) error(diags, file, b.get("zone"), "\"zone\" goes with touch_stick or touch_swipe");
    if (const JsonValue v = b.get("axis"); v.valid()) read_axis(v, def.axis, file, diags);
    if (def.axis == 1 && type != input::ActionType::Axis2D)
        error(diags, file, b.get("axis").valid() ? b.get("axis") : b, "\"axis\": \"y\" needs an axis2d action");
    f64 scale = 1.0;
    detail::read_number(b, "scale", 1.0, scale, file, kStage, diags);
    def.scale = static_cast<f32>(scale);
    bool invert = false;
    detail::read_bool(b, "invert", false, invert, file, kStage, diags);
    if (invert) def.flags |= input::BindingFlag::Invert;
    if (const JsonValue dz = b.get("deadzone"); dz.valid()) {
        if (!dz.is_array() || dz.size() != 2 || !dz.at(0).is_number() || !dz.at(1).is_number()) {
            error(diags, file, dz, "'deadzone' must be [inner, outer]");
        } else {
            def.dz.inner = static_cast<f32>(dz.at(0).as_number());
            def.dz.outer = static_cast<f32>(dz.at(1).as_number());
            if (def.dz.inner < 0.f || def.dz.inner >= def.dz.outer || def.dz.outer > 1.f)
                error(diags, file, dz, "'deadzone' must satisfy 0 <= inner < outer <= 1");
        }
    }
    if (const JsonValue c = b.get("curve"); c.valid()) {
        const StringView s = c.is_string() ? c.as_string() : StringView();
        if (s == "linear") def.curve = input::Curve::Linear;
        else if (s == "smooth") def.curve = input::Curve::Smooth;
        else if (s == "quadratic") def.curve = input::Curve::Quadratic;
        else if (s == "cubic") def.curve = input::Curve::Cubic;
        else error(diags, file, c, "'curve' must be linear, smooth, quadratic or cubic");
    }
    set.bindings.push_back(def);
}

void read_action(JsonValue a, input::InputMapSet& set, StringView file, DiagList& diags) {
    if (!a.is_object()) {
        error(diags, file, a, "an action must be an object");
        return;
    }
    if (!detail::check_keys(a, {"name", "type", "bindings"}, file, kStage, diags)) return;
    const JsonValue name = a.get("name");
    if (!name.is_string() || name.as_string().empty()) {
        error(diags, file, name.valid() ? name : a, "an action needs a non-empty \"name\"");
        return;
    }
    input::ActionDef def;
    def.name = StringId(name.as_string());
    if (const JsonValue t = a.get("type"); t.valid()) {
        const StringView s = t.is_string() ? t.as_string() : StringView();
        if (s == "button") def.type = input::ActionType::Button;
        else if (s == "axis1d") def.type = input::ActionType::Axis1D;
        else if (s == "axis2d") def.type = input::ActionType::Axis2D;
        else error(diags, file, t, "'type' must be button, axis1d or axis2d");
    }
    def.first_binding = static_cast<u16>(set.bindings.size());
    const JsonValue bindings = a.get("bindings");
    if (bindings.valid() && !bindings.is_array()) error(diags, file, bindings, "'bindings' must be an array");
    else if (bindings.valid())
        for (u32 i = 0; i < bindings.size(); ++i) read_binding(bindings.at(i), def.type, set, file, diags);
    def.binding_count = static_cast<u16>(set.bindings.size() - def.first_binding);
    set.actions.push_back(def);
    // The authored name rides along as the action's label (chunk v2), for options screens.
    set.labels.resize(set.actions.size());
    set.labels.back().assign(name.as_string().substr(0, input::kMaxActionLabel));
}

class InputMapImporter final : public IImporter {
public:
    StringView id() const override { return "input"; }
    u32 version() const override { return 2; }  // 2: action labels (chunk v2)
    ChunkType type() const override { return ChunkType::InputMap; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcinput"; }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        const u32 errors_before = diags.error_count();
        if (!detail::check_keys(doc, {"format", "version", "guid", "maps"}, file, kStage, diags)) return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-input") {
            error(diags, file, fmt.valid() ? fmt : doc, "\"format\" must be \"mce-input\"");
            return false;
        }
        const JsonValue ver = doc.get("version");
        if (!ver.is_integer() || ver.as_number() != kInputFormatVersion) {
            String msg;
            appendf(msg, "unsupported input map version (this cook reads version %u)", kInputFormatVersion);
            detail::error_at(diags, Errc::VersionTooNew, file, ver.valid() ? ver : doc, kStage, msg);
            return false;
        }
        const JsonValue maps = doc.get("maps");
        if (!maps.is_array() || maps.size() == 0) {
            error(diags, file, maps.valid() ? maps : doc, "'maps' must be a non-empty array");
            return false;
        }

        input::InputMapSet set;
        for (u32 m = 0; m < maps.size(); ++m) {
            const JsonValue map = maps.at(m);
            if (!map.is_object()) {
                error(diags, file, map, "a map must be an object");
                continue;
            }
            if (!detail::check_keys(map, {"name", "blocks_below", "actions"}, file, kStage, diags)) continue;
            const JsonValue name = map.get("name");
            if (!name.is_string() || name.as_string().empty()) {
                error(diags, file, name.valid() ? name : map, "a map needs a non-empty \"name\"");
                continue;
            }
            input::ActionMapDef def;
            def.name = StringId(name.as_string());
            bool blocks = false;
            detail::read_bool(map, "blocks_below", false, blocks, file, kStage, diags);
            if (blocks) def.flags |= input::MapFlag::BlocksBelow;
            def.first_action = static_cast<u16>(set.actions.size());
            const JsonValue actions = map.get("actions");
            if (actions.valid() && !actions.is_array()) error(diags, file, actions, "'actions' must be an array");
            else if (actions.valid())
                for (u32 a = 0; a < actions.size(); ++a) read_action(actions.at(a), set, file, diags);
            def.action_count = static_cast<u16>(set.actions.size() - def.first_action);
            set.maps.push_back(def);
        }
        if (diags.error_count() != errors_before) return false;

        Array<byte> bytes;
        input::encode_input_map(set, bytes);
        input::InputMapSet check;
        if (Result<void> r = input::decode_input_map(bytes.span(), check); !r) {
            FixedString<512> why;
            format_error(r.error(), why);
            String msg("the input map is not loadable: ");
            msg += why.view();
            error(diags, file, doc, msg);
            return false;
        }

        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::InputMap;
        d.version = asset::chunk_version(asset::INPUT_MAP_VERSION, 0);
        d.guid = in.guid;
        out.add(d, bytes.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_input_map_importer() { return mem::make_unique<InputMapImporter>(mem::heap()); }

}  // namespace mce::cook
