#include "mce/input/default_map.h"

namespace mce::input {

using platform::Key;

void build_default_map(InputMapBuilder& b) {
    b.map(MCE_SID("Gameplay"));
    b.action(MCE_SID("Move"), ActionType::Axis2D)
        .key(Key::D, 0, +1.f).key(Key::A, 0, -1.f).key(Key::W, 1, +1.f).key(Key::S, 1, -1.f)
        .key(Key::Right, 0, +1.f).key(Key::Left, 0, -1.f).key(Key::Up, 1, +1.f).key(Key::Down, 1, -1.f);
    b.action(MCE_SID("Look"), ActionType::Axis2D).mouse_delta(0).mouse_delta(1);
    b.action(MCE_SID("Jump")).key(Key::Space);
    b.action(MCE_SID("Fire")).key(Key::J).mouse_button(platform::MouseButton::Left);
    b.action(MCE_SID("Pause")).key(Key::Escape);

    b.map(MCE_SID("UI"), MapFlag::BlocksBelow);
    b.action(MCE_SID("Navigate"), ActionType::Axis2D)
        .key(Key::Right, 0, +1.f).key(Key::Left, 0, -1.f).key(Key::Up, 1, +1.f).key(Key::Down, 1, -1.f)
        .key(Key::D, 0, +1.f).key(Key::A, 0, -1.f).key(Key::W, 1, +1.f).key(Key::S, 1, -1.f);
    b.action(MCE_SID("Confirm")).key(Key::Return).key(Key::Space);
    b.action(MCE_SID("Cancel")).key(Key::Escape);
}

void default_map_chunk(Array<byte>& out) {
    InputMapBuilder b;
    build_default_map(b);
    b.encode(out);
}

}  // namespace mce::input
