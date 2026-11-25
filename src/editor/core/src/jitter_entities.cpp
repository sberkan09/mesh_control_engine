#include "mce/editor/jitter_entities.h"

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/editor/parents_first.h"

namespace mce::editor {

namespace {

constexpr f32 kPi = 3.14159265358979f;

// splitmix64: a few lines, the same numbers on every platform (std::uniform_real_distribution is not).
struct Rng {
    u64 state;
    u64 next() {
        u64 z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // A value in [0, 1].
    f32 unit() { return static_cast<f32>(next() >> 40) / 16777215.0f; }
    f32 between(f32 lo, f32 hi) { return lo + (hi - lo) * unit(); }
};

}  // namespace

bool jitter_params_valid(const JitterParams& p) {
    const f32 all[] = {p.move_x, p.move_y, p.rotate_degrees, p.scale_min, p.scale_max};
    for (f32 v : all)
        if (!std::isfinite(v)) return false;
    return p.move_x >= 0.0f && p.move_y >= 0.0f && p.rotate_degrees >= 0.0f && p.scale_min > 0.0f &&
           p.scale_min <= p.scale_max;
}

usize jitter_entities(EditorScene& scene, Span<const EntityId> ids, const JitterParams& p) {
    if (!jitter_params_valid(p)) return 0;
    if (p.move_x == 0.0f && p.move_y == 0.0f && p.rotate_degrees == 0.0f && p.scale_min == 1.0f && p.scale_max == 1.0f)
        return 0;
    struct Target {
        EntityId id;
        WorldTransform world;
    };
    Array<Target> moves;  // computed before anything moves: a parent that moves must not shift its children's target
    Rng rng{static_cast<u64>(p.seed) * 0x2545F4914F6CDD1Dull + 1};
    for (EntityId id : ids) {
        if (!scene.get<LocalTransform>(id)) continue;
        bool seen = false;
        for (const Target& t : moves) seen = seen || t.id == id;
        if (seen) continue;
        WorldTransform w = scene.world_transform(id);
        // Four draws per entity whatever the ranges are, so changing one range leaves the others' values alone.
        const f32 dx = rng.between(-p.move_x, p.move_x), dy = rng.between(-p.move_y, p.move_y);
        const f32 da = rng.between(-p.rotate_degrees, p.rotate_degrees) * (kPi / 180.0f);
        const f32 k = rng.between(p.scale_min, p.scale_max);
        w.position.x += dx;
        w.position.y += dy;
        w.rotation = rotation_z(angle_z(w.rotation) + da);
        w.scale.x *= k;
        w.scale.y *= k;
        w.scale.z *= k;
        moves.push_back({id, w});
    }
    if (moves.empty()) return 0;
    sort_parents_first(scene, moves, [](const Target& t) { return t.id; });
    scene.begin_edit("Randomize transforms");
    for (const Target& t : moves) scene.set(t.id, scene.local_for_world(t.id, t.world));
    scene.end_edit();
    return moves.size();
}

}  // namespace mce::editor
