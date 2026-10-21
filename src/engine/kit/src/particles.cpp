#include "mce/kit/particles.h"

#include <cmath>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/log.h"

namespace mce::kit {

namespace {

// PCG32 (O'Neill, XSH RR): small, fast and the same sequence on every platform.
u32 pcg_next(u64& state) {
    const u64 old = state;
    state = old * 6364136223846793005ull + 1442695040888963407ull;
    const u32 xorshifted = static_cast<u32>(((old >> 18u) ^ old) >> 27u);
    const u32 rot = static_cast<u32>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}
// [0, 1) with 24 bits: exact in f32.
f32 unit(u64& state) { return static_cast<f32>(pcg_next(state) >> 8) * (1.0f / 16777216.0f); }
f32 range(u64& state, const f32 r[2]) { return r[0] + (r[1] - r[0]) * unit(state); }

u64 seed_for(u32 seed, Entity e) {
    u64 s = mix64((u64(seed) << 32) ^ e.index);
    pcg_next(s);
    return s;
}

void particles_advance(Frame& f) {
    if (ParticleSystem* ps = f.try_resource<ParticleSystem>()) ps->advance(f.world(), f.dt);
}

}  // namespace

void ParticleSystem::set_preset(asset::Guid guid, const asset::ParticlePresetChunk& preset) {
    for (Registered& r : registered_)
        if (r.guid == guid) {
            r.preset = preset;
            for (Pool& p : pools_)
                if (p.preset_guid == guid) p.preset = preset;
            return;
        }
    registered_.push_back(Registered{guid, preset});
}

ParticleSystem::Pool* ParticleSystem::find(Entity e) {
    for (Pool& p : pools_)
        if (p.entity == e) return &p;
    return nullptr;
}

const ParticleSystem::Pool* ParticleSystem::find(Entity e) const {
    for (const Pool& p : pools_)
        if (p.entity == e) return &p;
    return nullptr;
}

ParticleSystem::Pool& ParticleSystem::find_or_add(Entity e) {
    if (Pool* p = find(e)) return *p;
    pools_.push_back(Pool{});
    pools_.back().entity = e;
    return pools_.back();
}

void ParticleSystem::burst(Entity emitter, u32 count) { find_or_add(emitter).pending_burst += count; }

u32 ParticleSystem::live_count(Entity emitter) const {
    const Pool* p = find(emitter);
    return p ? p->size() : 0;
}

void ParticleSystem::clear() {
    pools_.clear();
    stats_ = {};
}

// Copies the preset into the pool: registered presets first, then the mounted assets (the asset
// is released once copied). A preset that hot-reloaded (its generation moved) is read again.
bool ParticleSystem::resolve_preset(Pool& p, asset::Guid guid) {
    if (p.has_preset && p.preset_guid == guid &&
        (p.asset_key == asset::kInvalidAssetKey || !assets_ || assets_->generation(p.asset_key) == p.asset_generation))
        return true;
    p.preset_guid = guid;
    p.has_preset = false;
    p.asset_key = asset::kInvalidAssetKey;
    for (const Registered& r : registered_)
        if (r.guid == guid) {
            p.preset = r.preset;
            p.has_preset = true;
            return true;
        }
    if (!assets_ || guid == asset::Guid{}) return false;
    const asset::AssetKey key = assets_->find(guid);
    Result<asset::AssetHandle<asset::ParticlePreset>> h =
        key == asset::kInvalidAssetKey ? Result<asset::AssetHandle<asset::ParticlePreset>>(Error(Errc::NotFound, "kit.particles"))
                                       : assets_->load_sync<asset::ParticlePreset>(key);
    if (!h) {
        if (!p.warned) MCE_LOG_WARN("kit: particle preset {} cannot be loaded; its emitter stays empty", asset::to_string(guid).view());
        p.warned = true;
        return false;
    }
    Result<const asset::ParticlePresetChunk*> chunk = asset::ParticlePresetChunk::read(assets_->resolve(h.value()));
    if (chunk) {
        p.preset = *chunk.value();
        p.asset_key = key;
        p.asset_generation = h.value().generation;
        p.has_preset = true;
    }
    assets_->release(asset::AnyHandle(h.value()));
    return p.has_preset;
}

void ParticleSystem::spawn(Pool& p, math::vec2 origin, u32 count) {
    const asset::ParticlePresetChunk& c = p.preset;
    for (u32 i = 0; i < count; ++i) {
        if (p.size() >= c.max_particles) {
            stats_.dropped += count - i;
            return;
        }
        const f32 ox = c.area[0] * (2.0f * unit(p.rng) - 1.0f);
        const f32 oy = c.area[1] * (2.0f * unit(p.rng) - 1.0f);
        const f32 angle = c.direction + c.spread * (2.0f * unit(p.rng) - 1.0f);
        const f32 speed = range(p.rng, c.speed);
        p.pos.push_back({origin.x + ox, origin.y + oy});
        p.vel.push_back({std::cos(angle) * speed, std::sin(angle) * speed});
        p.age.push_back(0.0f);
        p.life.push_back(math::max(range(p.rng, c.lifetime), 1e-4f));
        p.rot.push_back(0.0f);
        p.spin.push_back(range(p.rng, c.spin));
        ++stats_.spawned;
    }
}

void ParticleSystem::advance(World& world, f32 dt) {
    stats_.spawned = stats_.dropped = 0;
    for (Pool& p : pools_) p.used = false;
    Array<Entity> finished;

    auto step = [&](Entity e, math::vec3 position, const ParticleEmitter& em) {
        Pool& p = find_or_add(e);
        p.used = true;
        p.order = em.order;
        p.layer = em.layer;
        if (!resolve_preset(p, em.preset.guid)) return;
        const asset::ParticlePresetChunk& c = p.preset;

        // Integrate and retire (swap-remove keeps the order deterministic).
        const math::vec2 g{c.gravity[0] * dt, c.gravity[1] * dt};
        const f32 damp = 1.0f / (1.0f + c.drag * dt);
        for (u32 i = 0; i < p.size();) {
            p.age[i] += dt;
            if (p.age[i] >= p.life[i]) {
                const u32 last = p.size() - 1;
                p.pos[i] = p.pos[last];
                p.vel[i] = p.vel[last];
                p.age[i] = p.age[last];
                p.life[i] = p.life[last];
                p.rot[i] = p.rot[last];
                p.spin[i] = p.spin[last];
                p.pos.pop_back();
                p.vel.pop_back();
                p.age.pop_back();
                p.life.pop_back();
                p.rot.pop_back();
                p.spin.pop_back();
                continue;
            }
            p.vel[i] = (p.vel[i] + g) * damp;
            p.pos[i] = p.pos[i] + p.vel[i] * dt;
            p.rot[i] += p.spin[i] * dt;
            ++i;
        }

        // Emit: the start burst, requested bursts, then the rate.
        const math::vec2 origin{position.x, position.y};
        if (!p.started && em.playing) {
            p.started = true;
            p.rng = seed_for(em.seed, e);
            spawn(p, origin, c.burst);
        }
        if (p.pending_burst) {
            if (p.rng == 0) p.rng = seed_for(em.seed, e);
            spawn(p, origin, p.pending_burst);
            p.pending_burst = 0;
        }
        const bool emitting = p.started && em.playing && (c.duration <= 0.0f ? c.rate > 0.0f : p.elapsed < c.duration);
        if (emitting) {
            p.carry += c.rate * dt;
            const u32 n = static_cast<u32>(p.carry);
            p.carry -= static_cast<f32>(n);
            spawn(p, origin, n);
            p.elapsed += dt;
        }
        if (em.destroy_when_done && p.started && !emitting && p.size() == 0) finished.push_back(e);
    };
    world.query<const LocalTransform, const ParticleEmitter>().without<WorldTransform>().each(
        [&](Entity e, const LocalTransform& t, const ParticleEmitter& em) { step(e, t.position, em); });
    world.query<const WorldTransform, const ParticleEmitter>().each(
        [&](Entity e, const WorldTransform& t, const ParticleEmitter& em) { step(e, t.position, em); });

    // Emitters that went away take their particles with them.
    for (usize i = pools_.size(); i-- > 0;) {
        if (!pools_[i].used) pools_.erase(i);
    }
    for (Entity e : finished) {
        if (world.in_phase()) world.commands().destroy(e);
        else world.destroy(e);
    }
    stats_.emitters = static_cast<u32>(pools_.size());
    stats_.live = 0;
    for (const Pool& p : pools_) stats_.live += p.size();
}

void ParticleSystem::draw(render::SpriteBatch& batch, ResolveSet resolve, void* user) const {
    for (const Pool& p : pools_) {
        if (!p.has_preset || p.size() == 0) continue;
        const asset::ParticlePresetChunk& c = p.preset;
        const render::SpriteSetId set = resolve(user, c.texture_guid());
        const render::SpriteBlend blend =
            (c.flags & asset::ParticleFlags::Additive) ? render::SpriteBlend::Additive : render::SpriteBlend::Alpha;
        for (u32 i = 0; i < p.size(); ++i) {
            const f32 t = math::clamp(p.age[i] / p.life[i], 0.0f, 1.0f);
            const f32 size = math::lerp(c.size_start, c.size_end, t);
            render::SpriteInstance s;
            s.position = p.pos[i];
            s.rotation = p.rot[i];
            s.scale = {size, size};
            s.tint = math::Color{math::lerp(c.color_start[0], c.color_end[0], t), math::lerp(c.color_start[1], c.color_end[1], t),
                                 math::lerp(c.color_start[2], c.color_end[2], t), math::lerp(c.color_start[3], c.color_end[3], t)};
            s.set = set;
            s.sort_layer = p.layer;
            s.sort_order = p.order;
            s.blend = blend;
            batch.add(s);
        }
    }
}

u64 ParticleSystem::hash(u64 seed) const {
    Hasher64 h(seed);
    h.u32v(static_cast<u32>(pools_.size()));
    for (const Pool& p : pools_) {
        h.u32v(p.entity.index);
        h.u32v(p.entity.generation);
        h.u64v(p.rng);
        h.f32v(p.elapsed);
        h.f32v(p.carry);
        h.u32v(p.size());
        h.bytes(p.pos.data(), p.pos.size() * sizeof(math::vec2));
        h.bytes(p.vel.data(), p.vel.size() * sizeof(math::vec2));
        h.bytes(p.age.data(), p.age.size() * sizeof(f32));
        h.bytes(p.rot.data(), p.rot.size() * sizeof(f32));
    }
    return h.value();
}

void install_particles(World& world, asset::AssetSystem* assets) {
    if (ParticleSystem* ps = world.try_resource<ParticleSystem>()) {
        ps->set_assets(assets);
        return;
    }
    world.register_components<ParticleEmitter>();
    world.set_resource(ParticleSystem(assets));
    world.add_system(Phase::FixedUpdate_Post, "particles_advance", &particles_advance);
    world.run_during_cutscenes(Phase::FixedUpdate_Post, "particles_advance");
}

}  // namespace mce::kit
