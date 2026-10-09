#pragma once
// CPU particles (FR-R2D-070, ecs-gameplay §2.9): the ParticleEmitter starter-kit component, the
// reference simulation, and its sprite output. The renderer has no particle code: every live
// particle is drawn as a SpriteInstance through the one sprite batch (rhi-renderer §3.4).
//
//   kit::install_particles(world, &assets);   // the ParticleSystem resource + "particles_advance"
//   ... an entity with LocalTransform + ParticleEmitter{preset} emits from its position
//   world.resource<kit::ParticleSystem>().burst(entity, 40);   // gameplay one-shots (dust, hits)
//
// The simulation runs on the fixed tick (FixedUpdate_Post, after physics, so emitters attached to
// bodies see final poses) and is deterministic: each emitter owns a PCG stream seeded from its
// `seed` and entity index, particles live in world space (they do not follow the emitter) and
// die in swap-remove order. ParticleSystem::hash() covers every live particle, so replays and
// the determinism harness can assert it alongside World::state_hash.

#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/ecs/world.h"
#include "mce/kit/components.h"
#include "mce/render/sprite_batch.h"

namespace mce::kit {

struct ParticleStats {
    u32 emitters = 0;  // emitters with simulation state
    u32 live = 0;      // particles alive after the last advance
    u32 spawned = 0;   // during the last advance
    u32 dropped = 0;   // emission refused by max_particles during the last advance
};

class ParticleSystem {
public:
    ParticleSystem() = default;
    explicit ParticleSystem(asset::AssetSystem* assets) : assets_(assets) {}

    // Presets resolve through the mounted assets (hot reload re-reads them), or through presets
    // registered here by GUID (tests, code-built effects), which take precedence.
    void set_assets(asset::AssetSystem* assets) { assets_ = assets; }
    void set_preset(asset::Guid guid, const asset::ParticlePresetChunk& preset);

    // Queues `count` extra particles from `emitter` on the next advance (on top of its rate).
    void burst(Entity emitter, u32 count);

    // One fixed step: spawns, integrates and retires particles for every ParticleEmitter in ECS
    // order; emitters that went away drop their particles. Called by "particles_advance".
    void advance(World& world, f32 dt);

    // Draws every live particle into `batch` (culling happens there). `resolve` maps a preset's
    // texture GUID to a sprite set; SpriteExtract passes its texture cache.
    using ResolveSet = render::SpriteSetId (*)(void* user, asset::Guid texture);
    void draw(render::SpriteBatch& batch, ResolveSet resolve, void* user) const;

    // Folds every emitter's live particles (positions, velocities, ages, rng state) into a hash.
    u64 hash(u64 seed = kFnv64Offset) const;
    const ParticleStats& stats() const { return stats_; }
    u32 live_count(Entity emitter) const;
    void clear();

private:
    struct Pool {
        Entity entity;
        asset::Guid preset_guid;
        asset::AssetKey asset_key = asset::kInvalidAssetKey;  // where the copied preset came from
        u32 asset_generation = 0;    // re-read when the asset hot-reloads
        asset::ParticlePresetChunk preset{};
        u64 rng = 0;
        f32 elapsed = 0.0f;          // seconds since emission started
        f32 carry = 0.0f;            // fractional particles owed by `rate`
        u32 pending_burst = 0;
        i32 order = 0;
        i16 layer = 0;
        bool started = false;
        bool has_preset = false;
        bool used = false;
        bool warned = false;
        // structure of arrays, one entry per live particle
        Array<math::vec2> pos;
        Array<math::vec2> vel;
        Array<f32> age;
        Array<f32> life;
        Array<f32> rot;
        Array<f32> spin;
        u32 size() const { return static_cast<u32>(age.size()); }
    };

    Pool* find(Entity e);
    const Pool* find(Entity e) const;
    Pool& find_or_add(Entity e);
    bool resolve_preset(Pool& p, asset::Guid guid);
    void spawn(Pool& p, math::vec2 origin, u32 count);

    asset::AssetSystem* assets_ = nullptr;
    struct Registered {
        asset::Guid guid;
        asset::ParticlePresetChunk preset;
    };
    Array<Registered> registered_;
    Array<Pool> pools_;
    ParticleStats stats_;
};

// Registers ParticleEmitter, sets the ParticleSystem resource and adds "particles_advance" to
// FixedUpdate_Post (after "transform_propagate" when the hierarchy is installed). Idempotent.
void install_particles(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
