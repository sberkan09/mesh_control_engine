#pragma once
// CameraBookmarks — nine saved viewpoints (M-CAMBOOKMARK): Ctrl+1..9 stores the viewport's camera in a slot and 1..9
// flies back to it. A slot holds both views at once, the 2D camera (centre, zoom) and the 3D orbit camera (target,
// yaw, pitch, distance), so the same key works in either mode. Saved as one line per set slot. Headless, tested in CI.

#include "mce/cook/string.h"
#include "mce/editor/orbit_camera.h"
#include "mce/editor/viewport2d.h"

namespace mce::editor {

struct CameraBookmark {
    bool set = false;
    math::vec2 center;  // the 2D camera
    f32 zoom = 1.0f;
    math::vec3 target;  // the orbit camera
    f32 yaw = 0.0f;
    f32 pitch = 0.0f;
    f32 distance = 12.0f;
};

class CameraBookmarks {
public:
    static constexpr u32 kSlots = 9;  // slot 0 is shown and typed as "1"

    // Stores the two cameras in `slot` (< kSlots; ignored beyond).
    void save(u32 slot, const EditorCamera2D& cam2d, const OrbitCamera& orbit);
    // Writes the slot's view into both cameras; false (nothing changed) when the slot is empty.
    bool recall(u32 slot, EditorCamera2D& cam2d, OrbitCamera& orbit) const;
    bool has(u32 slot) const { return slot < kSlots && slots_[slot].set; }
    void clear(u32 slot) {
        if (slot < kSlots) slots_[slot] = CameraBookmark{};
    }
    void clear_all() {
        for (CameraBookmark& b : slots_) b = CameraBookmark{};
    }
    u32 count() const;

    // One line per set slot: "<1..9> cx cy zoom tx ty tz yaw pitch distance".
    cook::String serialize() const;
    // Replaces every slot with the lines of `text`; malformed lines, slots out of 1..9 and non-finite numbers are
    // skipped.
    void parse(StringView text);

private:
    CameraBookmark slots_[kSlots];
};

}  // namespace mce::editor
