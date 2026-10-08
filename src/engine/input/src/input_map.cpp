#include "mce/input/input_map.h"

#include <cstring>

namespace mce::input {

namespace {

template <class T>
void append_pod(Array<byte>& out, const T* items, usize count) {
    const usize bytes = sizeof(T) * count;
    const usize at = out.size();
    out.resize(at + bytes);
    if (bytes) std::memcpy(out.data() + at, items, bytes);
}

template <class T>
bool read_pod(Span<const byte> chunk, usize& pos, usize count, Array<T>& out) {
    const usize bytes = sizeof(T) * count;
    if (chunk.size() - pos < bytes) return false;
    out.resize(count);
    if (bytes) std::memcpy(out.data(), chunk.data() + pos, bytes);
    pos += bytes;
    return true;
}

}  // namespace

const ActionMapDef* InputMapSet::find_map(StringId name) const {
    for (const ActionMapDef& m : maps)
        if (m.name == name) return &m;
    return nullptr;
}

Result<void> decode_input_map(Span<const byte> chunk, InputMapSet& out) {
    constexpr const char* kStage = "input.map.decode";
    InputMapHeader h;
    if (chunk.size() < sizeof(h)) return Error(Errc::Truncated, kStage);
    std::memcpy(&h, chunk.data(), sizeof(h));
    if (h.magic != kInputMapMagic) return Error(Errc::BadMagic, kStage, "", h.magic);
    if (h.version > kInputMapVersion) return Error(Errc::VersionTooNew, kStage, "", h.version);
    if (h.map_count == 0) return Error(Errc::InvalidArgument, kStage, "no action maps");

    usize pos = sizeof(h);
    if (!read_pod(chunk, pos, h.map_count, out.maps) || !read_pod(chunk, pos, h.action_count, out.actions) ||
        !read_pod(chunk, pos, h.binding_count, out.bindings))
        return Error(Errc::Truncated, kStage);

    for (usize i = 0; i < out.maps.size(); ++i) {
        const ActionMapDef& m = out.maps[i];
        if (u32(m.first_action) + m.action_count > h.action_count)
            return Error(Errc::InvalidArgument, kStage, "map action range out of bounds", static_cast<u32>(i));
        if (m.action_count > kMaxActions)
            return Error(Errc::InvalidArgument, kStage, "map has more than kMaxActions actions", static_cast<u32>(i));
        for (usize j = 0; j < i; ++j)
            if (out.maps[j].name == m.name)
                return Error(Errc::InvalidArgument, kStage, "duplicate map name", static_cast<u32>(i));
    }
    for (usize i = 0; i < out.actions.size(); ++i) {
        const ActionDef& a = out.actions[i];
        if (u32(a.first_binding) + a.binding_count > h.binding_count)
            return Error(Errc::InvalidArgument, kStage, "action binding range out of bounds", static_cast<u32>(i));
        if (static_cast<u8>(a.type) > static_cast<u8>(ActionType::Axis2D))
            return Error(Errc::InvalidArgument, kStage, "bad action type", static_cast<u32>(i));
    }
    for (usize i = 0; i < out.bindings.size(); ++i) {
        const BindingDef& b = out.bindings[i];
        if (static_cast<u8>(b.device) > static_cast<u8>(Device::GamepadAxis) || b.axis > 1 ||
            static_cast<u8>(b.curve) > static_cast<u8>(Curve::Cubic) || !(b.dz.outer > b.dz.inner))
            return Error(Errc::InvalidArgument, kStage, "bad binding", static_cast<u32>(i));
    }
    return ok();
}

void encode_input_map(const InputMapSet& set, Array<byte>& out) {
    InputMapHeader h;
    h.map_count = static_cast<u32>(set.maps.size());
    h.action_count = static_cast<u32>(set.actions.size());
    h.binding_count = static_cast<u32>(set.bindings.size());
    out.clear();
    append_pod(out, &h, 1);
    append_pod(out, set.maps.data(), set.maps.size());
    append_pod(out, set.actions.data(), set.actions.size());
    append_pod(out, set.bindings.data(), set.bindings.size());
}

// ---------------------------------------------------------------------------- builder

InputMapBuilder& InputMapBuilder::map(StringId name, u32 flags) {
    ActionMapDef m;
    m.name = name;
    m.first_action = static_cast<u16>(set_.actions.size());
    m.flags = flags;
    set_.maps.push_back(m);
    return *this;
}

InputMapBuilder& InputMapBuilder::action(StringId name, ActionType type) {
    MCE_ASSERT_MSG(!set_.maps.empty(), "InputMapBuilder::action before map()");
    ActionDef a;
    a.name = name;
    a.type = type;
    a.first_binding = static_cast<u16>(set_.bindings.size());
    set_.actions.push_back(a);
    ++set_.maps.back().action_count;
    return *this;
}

InputMapBuilder& InputMapBuilder::bind(const BindingDef& b) {
    MCE_ASSERT_MSG(!set_.actions.empty(), "InputMapBuilder binding before action()");
    set_.bindings.push_back(b);
    ++set_.actions.back().binding_count;
    return *this;
}

InputMapBuilder& InputMapBuilder::key(platform::Key k, u8 axis, f32 scale) {
    BindingDef b;
    b.device = Device::Keyboard;
    b.code = static_cast<u32>(k);
    b.axis = axis;
    b.scale = scale;
    return bind(b);
}

InputMapBuilder& InputMapBuilder::mouse_button(platform::MouseButton mb) {
    BindingDef b;
    b.device = Device::MouseButton;
    b.code = static_cast<u32>(mb);
    return bind(b);
}

InputMapBuilder& InputMapBuilder::mouse_delta(u8 axis, f32 scale) {
    BindingDef b;
    b.device = Device::MouseDelta;
    b.axis = axis;
    b.scale = scale;
    return bind(b);
}

InputMapBuilder& InputMapBuilder::mouse_wheel(u8 axis, f32 scale) {
    BindingDef b;
    b.device = Device::MouseWheel;
    b.axis = axis;
    b.scale = scale;
    return bind(b);
}

InputMapBuilder& InputMapBuilder::gamepad_button(platform::GamepadButton btn, u8 axis, f32 scale) {
    BindingDef b;
    b.device = Device::GamepadButton;
    b.code = static_cast<u32>(btn);
    b.axis = axis;
    b.scale = scale;
    return bind(b);
}

InputMapBuilder& InputMapBuilder::gamepad_axis(platform::GamepadAxis a, u8 axis, f32 scale, Deadzone dz, Curve curve) {
    BindingDef b;
    b.device = Device::GamepadAxis;
    b.code = static_cast<u32>(a);
    b.axis = axis;
    b.scale = scale;
    b.dz = dz;
    b.curve = curve;
    return bind(b);
}

}  // namespace mce::input
