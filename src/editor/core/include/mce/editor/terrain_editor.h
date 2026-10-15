#pragma once
// Terrain sculpting and painting (M-TERRAIN 4).
//
//   TerrainDocument  a .mcterrain being edited: the heights (metres), the splat weights and the hole
//                    bits as the cook made them (auto rules applied), plus the source document's
//                    other keys, kept as written. Brushes change the data; serialize() writes the
//                    source back with explicit `heights` (and `splat` / `holes` once painted).
//   TerrainEditor    what the Terrain panel and the viewport's sculpt tool drive: binds to the
//                    selected entity's Terrain3D, applies brush dabs in strokes, re-bakes the asset
//                    during a stroke (flush) and at its end, and undoes and redoes whole strokes (the
//                    terrain lives in the asset, not in the scene document).
// Headless: tested in CI; the panel and the viewport draw it.

#include "mce/cook/string.h"
#include "mce/editor/editor_session.h"
#include "mce/math/math.h"

namespace mce::editor {

enum class TerrainBrushOp : u32 {
    Raise,    // adds `strength` metres at the centre per dab, fading to the rim
    Lower,    // the opposite
    Smooth,   // pulls each sample toward its neighbours' mean, by `strength` (0..1) per dab
    Flatten,  // pulls each sample toward `target` metres, by `strength` (0..1) per dab
    Noise,    // adds value noise of `strength` metres (fixed per position and `seed`) per dab
    Paint,    // raises splat layer `layer` by `strength` (0..1) of full weight per dab
    Hole,     // cuts the samples inside the radius out (drawn and collided)
    Fill,     // closes holes inside the radius again
};

struct TerrainBrush {
    TerrainBrushOp op = TerrainBrushOp::Raise;
    f32 radius = 4.0f;    // metres, in the terrain's own space
    f32 strength = 0.5f;  // see the ops
    f32 hardness = 0.5f;  // 0: falls off from the centre, 1: full weight up to the rim
    u32 layer = 0;        // Paint
    f32 target = 0.0f;    // Flatten, metres
    u32 seed = 1;         // Noise
};

// Brush weight at `d` metres from the centre: 1 inside `hardness` of the radius, a smooth fall to 0
// at the rim.
f32 terrain_brush_falloff(const TerrainBrush& b, f32 d);

class TerrainDocument {
public:
    // `source` is the .mcterrain text, `cooked` its CHNK_TERRAIN payload. False (with a diagnostic)
    // when either does not read.
    bool load(StringView source, Span<const byte> cooked, StringView file, cook::DiagList& diags);
    void serialize(cook::String& out) const;

    bool loaded() const { return resolution != 0; }
    u32 resolution = 0;
    u32 splat_resolution = 0;
    u32 layer_count = 0;
    math::vec2 size{};
    f32 height_min = 0.0f, height_max = 1.0f;
    Array<f32> heights;  // metres, resolution² (z rows of x columns), inside [height_min, height_max]
    Array<u8> weights;   // splat_resolution² × layer_count, sample by sample (the source's layout)
    Array<u8> holes;     // a bit per height sample, (resolution² + 7) / 8 bytes

    math::vec2 spacing() const;
    f32 height(u32 x, u32 z) const { return heights[usize(z) * resolution + x]; }
    bool hole(u32 x, u32 z) const;
    u8 weight(u32 x, u32 z, u32 layer) const { return weights[(usize(z) * splat_resolution + x) * layer_count + layer]; }
    // The bilinear height at (x, z) metres in the terrain's space, clamped to its edges.
    f32 height_at(f32 x, f32 z) const;
    // Applies one dab of `b` centred on (x, z) metres in the terrain's space, `scale` times its
    // strength (a frame's share). Returns the samples (height, weight or hole) it changed.
    u32 apply(const TerrainBrush& b, math::vec2 center, f32 scale = 1.0f);

private:
    cook::String kept_;            // the source's other keys, one `  "key": value,` line each
    bool splat_explicit_ = false;  // the source had `splat`, or a Paint dab ran
};

class TerrainEditor {
public:
    static constexpr usize kMaxHistoryBytes = 64 * 1024 * 1024;

    // Follows the selection: binds to `entity`'s Terrain3D (re-reading the asset when it changed).
    // False when the entity has none, or the asset cannot be read.
    bool bind(EditorSession& session, EntityId entity);
    bool bound() const { return doc_.loaded(); }
    void unbind();
    EntityId entity() const { return entity_; }
    asset::Guid terrain() const { return terrain_; }
    const TerrainDocument& doc() const { return doc_; }

    // The world-space ray's first hit on the terrain's surface (holes ignored), in world space and
    // in the terrain's own space. Marches the bilinear surface under the entity's transform.
    bool raycast(const EditorScene& scene, math::vec3 origin, math::vec3 dir, math::vec3& world_hit, math::vec2& local_hit) const;
    // The brush's rim around `local` as world-space segments draped on the surface.
    void brush_outline(const EditorScene& scene, math::vec2 local, f32 radius, Array<math::vec3>& points) const;

    // A stroke groups dabs into one undo step. flush() re-bakes mid-stroke so the viewport follows.
    void begin_stroke();
    u32 dab(math::vec2 local, f32 scale = 1.0f);
    Result<f64> flush(EditorSession& session);
    // Saves and hot-reloads when the stroke changed anything; returns the reload's wall time.
    Result<f64> end_stroke(EditorSession& session);
    bool in_stroke() const { return stroking_; }
    bool dirty() const { return dirty_; }

    bool undo(EditorSession& session);
    bool redo(EditorSession& session);
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }

    TerrainBrush brush;

private:
    struct Snapshot {
        Array<f32> heights;
        Array<u8> weights;
        Array<u8> holes;
        usize bytes() const { return heights.size() * sizeof(f32) + weights.size() + holes.size(); }
    };
    Snapshot snapshot() const;
    void restore(Snapshot& s);
    Result<f64> save(EditorSession& session);

    EntityId entity_ = kNoEntity;
    asset::Guid terrain_;
    cook::String path_;  // absolute
    TerrainDocument doc_;
    Snapshot before_;
    bool stroking_ = false;
    bool changed_ = false;  // this stroke changed something
    bool dirty_ = false;    // changed since the last save
    Array<Snapshot> undo_, redo_;
};

}  // namespace mce::editor
