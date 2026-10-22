#include "mce/editor/camera_bookmarks.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mce::editor {

void CameraBookmarks::save(u32 slot, const EditorCamera2D& cam2d, const OrbitCamera& orbit) {
    if (slot >= kSlots) return;
    CameraBookmark& b = slots_[slot];
    b.set = true;
    b.center = cam2d.center;
    b.zoom = cam2d.zoom;
    b.target = orbit.target;
    b.yaw = orbit.yaw;
    b.pitch = orbit.pitch;
    b.distance = orbit.distance;
}

bool CameraBookmarks::recall(u32 slot, EditorCamera2D& cam2d, OrbitCamera& orbit) const {
    if (!has(slot)) return false;
    const CameraBookmark& b = slots_[slot];
    cam2d.center = b.center;
    cam2d.zoom = b.zoom;
    orbit.target = b.target;
    orbit.yaw = b.yaw;
    orbit.pitch = b.pitch;
    orbit.distance = b.distance;
    orbit.effective = b.distance;
    return true;
}

u32 CameraBookmarks::count() const {
    u32 n = 0;
    for (const CameraBookmark& b : slots_) n += b.set ? 1u : 0u;
    return n;
}

cook::String CameraBookmarks::serialize() const {
    cook::String out;
    for (u32 i = 0; i < kSlots; ++i) {
        const CameraBookmark& b = slots_[i];
        if (!b.set) continue;
        char line[256];
        std::snprintf(line, sizeof(line), "%u %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", i + 1,
                      double(b.center.x), double(b.center.y), double(b.zoom), double(b.target.x), double(b.target.y),
                      double(b.target.z), double(b.yaw), double(b.pitch), double(b.distance));
        out += StringView(line);
    }
    return out;
}

void CameraBookmarks::parse(StringView text) {
    clear_all();
    usize pos = 0;
    while (pos < text.size()) {
        usize end = pos;
        while (end < text.size() && text[end] != '\n') ++end;
        char line[256];
        const usize len = end - pos < sizeof(line) - 1 ? end - pos : sizeof(line) - 1;
        for (usize i = 0; i < len; ++i) line[i] = text[pos + i];
        line[len] = '\0';
        pos = end + 1;

        // Ten numbers: the slot, then nine floats.
        double v[10];
        const char* p = line;
        u32 n = 0;
        for (; n < 10; ++n) {
            char* next = nullptr;
            v[n] = std::strtod(p, &next);
            if (next == p || !std::isfinite(v[n])) break;
            p = next;
        }
        while (*p == ' ' || *p == '\r' || *p == '\t') ++p;
        if (n != 10 || *p != '\0') continue;
        if (v[0] < 1.0 || v[0] > double(kSlots) || v[0] != std::floor(v[0])) continue;
        CameraBookmark& b = slots_[static_cast<u32>(v[0]) - 1];
        b.set = true;
        b.center = {static_cast<f32>(v[1]), static_cast<f32>(v[2])};
        b.zoom = static_cast<f32>(v[3]);
        b.target = {static_cast<f32>(v[4]), static_cast<f32>(v[5]), static_cast<f32>(v[6])};
        b.yaw = static_cast<f32>(v[7]);
        b.pitch = static_cast<f32>(v[8]);
        b.distance = static_cast<f32>(v[9]);
    }
}

}  // namespace mce::editor
