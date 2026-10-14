// Taps and clicks for the novel stage and NPC dialogue (novel.h, M-MOBILE 11f): where the pointer
// pressed this tick, and the dialogue box's rectangles the sim hit-tests (the box in
// dialogue_box.cpp anchors its widgets by the same numbers).
#include "mce/input/actions.h"
#include "mce/kit/components.h"
#include "mce/kit/novel.h"

namespace mce::novel {

bool pointer_pressed(const input::InputState& in, const kit::ViewInfo& view, math::vec2& px) {
    for (const input::TouchPoint& t : in.touch_points())
        if (t.pressed) {
            px = {t.pos.x * static_cast<f32>(view.framebuffer_width), t.pos.y * static_cast<f32>(view.framebuffer_height)};
            return true;
        }
    constexpr StringId kPoint = MCE_SID("Point");
    if (in.has_action(kPoint) && in.pressed(kPoint)) {
        const math::vec2 m = in.mouse_pos_px();
        const f32 sx = view.window_width ? static_cast<f32>(view.framebuffer_width) / static_cast<f32>(view.window_width) : 1.0f;
        const f32 sy = view.window_height ? static_cast<f32>(view.framebuffer_height) / static_cast<f32>(view.window_height) : 1.0f;
        px = {m.x * sx, m.y * sy};
        return true;
    }
    return false;
}

DialogueRects dialogue_rects(const DialogueLayout& layout, const kit::ViewInfo& view) {
    // The box's anchors (dialogue_box.cpp): the safe area is the root, offsets scale with the UI.
    const f32 s = view.ui_scale > 0.0f ? view.ui_scale : 1.0f;
    const f32 left = static_cast<f32>(view.safe_left);
    const f32 right = static_cast<f32>(view.framebuffer_width) - static_cast<f32>(view.safe_right);
    const f32 bottom = static_cast<f32>(view.framebuffer_height) - static_cast<f32>(view.safe_bottom);
    const f32 h = layout.box_height, row_h = layout.text_px + 18.0f, list_h = 4 * row_h + 3 * 10.0f;
    DialogueRects r;
    r.box = {left + 24 * s, bottom - (h + 24) * s, right - 24 * s, bottom - 24 * s};
    const f32 cx = left + 0.5f * (right - left);
    const f32 list_top = bottom - (h + 48 + list_h) * s;
    for (u32 i = 0; i < 4; ++i) {
        const f32 y = list_top + static_cast<f32>(i) * (row_h + 10.0f) * s;
        r.rows[i] = {cx - 360 * s, y, cx + 360 * s, y + row_h * s};
    }
    return r;
}

}  // namespace mce::novel
