#include "mce/scene/streaming.h"

#include <cmath>

#include "mce/ecs/hierarchy.h"
#include "mce/log.h"
#include "mce/scene/scene.h"

namespace mce::scene {

namespace {

constexpr f32 kFar = 3.0e38f;

// A root's LocalTransform is current even between ticks; a child's world position is the last
// propagated one.
math::vec3 position_of(const World& world, Entity e) {
    if (world.has<ChildOf>(e))
        if (const WorldTransform* w = world.try_get<WorldTransform>(e)) return w->position;
    if (const LocalTransform* l = world.try_get<LocalTransform>(e)) return l->position;
    return {};
}

struct ChunkView {
    Entity entity;
    SceneChunk chunk;
    math::vec3 centre;
};

}  // namespace

f32 distance_to_box(const math::vec3& p, const math::vec3& centre, const math::vec3& half) {
    const f32 dx = math::max(std::fabs(p.x - centre.x) - std::fabs(half.x), 0.0f);
    const f32 dy = math::max(std::fabs(p.y - centre.y) - std::fabs(half.y), 0.0f);
    const f32 dz = math::max(std::fabs(p.z - centre.z) - std::fabs(half.z), 0.0f);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

SceneStreaming::SceneStreaming(asset::AssetSystem* assets, StreamingConfig config) : assets_(assets), config_(config) {}

SceneStreaming::~SceneStreaming() {
    for (Record& r : records_) cancel(r);
}

SceneStreaming::SceneStreaming(SceneStreaming&& o) noexcept = default;
SceneStreaming& SceneStreaming::operator=(SceneStreaming&& o) noexcept = default;

SceneStreaming::Record* SceneStreaming::find(Entity chunk) {
    for (Record& r : records_)
        if (r.chunk == chunk) return &r;
    return nullptr;
}

const SceneStreaming::Record* SceneStreaming::find(Entity chunk) const {
    return const_cast<SceneStreaming*>(this)->find(chunk);
}

Span<const Entity> SceneStreaming::entities(Entity chunk) const {
    const Record* r = find(chunk);
    return r ? r->spawned.span() : Span<const Entity>();
}

void SceneStreaming::cancel(Record& r) {
    if (r.state != ChunkState::Loading) return;
    if (assets_ && r.ticket.valid()) assets_->cancel_async(r.ticket);
    r.ticket = {};
    r.state = ChunkState::Unloaded;
}

void SceneStreaming::unload(World& world, Record& r) {
    if (r.state == ChunkState::Loading) cancel(r);
    if (r.state == ChunkState::Loaded) {
        // Children go with their parents (the hierarchy's destroy hook), so check before each.
        for (Entity e : r.spawned)
            if (world.alive(e)) world.destroy(e);
        ++stats_.total_unloads;
    }
    r.spawned.clear();
    r.state = ChunkState::Unloaded;
}

void SceneStreaming::fail(Record& r, Errc code) {
    MCE_LOG_WARN("scene streaming: chunk scene {} failed to load ({})", asset::to_string(r.scene).view(), errc_name(code));
    r.ticket = {};
    r.spawned.clear();
    r.state = ChunkState::Failed;
}

void SceneStreaming::instantiate(World& world, Record& r, asset::AnyHandle handle) {
    const asset::AssetHandle<asset::Scene> h{handle.index, handle.generation};
    Result<asset::SceneView> view = asset::SceneView::read(assets_->resolve(h));
    Result<void> made = view ? scene::instantiate(world, view.value(), &r.spawned) : Result<void>(view.error());
    assets_->release(handle);  // the entities keep nothing of the scene blob
    r.ticket = {};
    if (!made) {
        fail(r, made.error().code);
        return;
    }
    r.state = ChunkState::Loaded;
    ++stats_.total_loads;
}

void SceneStreaming::unload_all(World& world) {
    for (Record& r : records_) unload(world, r);
}

void SceneStreaming::pump(World& world, PumpMode mode) {
    MCE_ASSERT_MSG(!world.in_phase(), "pump_streaming spawns and destroys: call it between ticks");
    // A world that never saw a SceneChunk has nothing to stream, and stays exactly as it was:
    // not even the streaming components get registered.
    if (!assets_ || (records_.empty() && world.find_component(kTypeHash<SceneChunk>) == ecs::kInvalidComponent)) return;
    const bool settle = mode == PumpMode::Settle;
    const bool sync = settle || !config_.async;

    // Gather first: loading and unloading change the world under a live query.
    Array<math::vec3> focus;
    world.query<const StreamingFocus>().each([&](Entity e, const StreamingFocus& f) {
        if (f.enabled) focus.push_back(position_of(world, e));
    });
    Array<ChunkView> chunks;
    world.query<const SceneChunk>().each(
        [&](Entity e, const SceneChunk& c) { chunks.push_back(ChunkView{e, c, position_of(world, e)}); });

    // Chunks that went away (entity destroyed or SceneChunk removed) take their entities along.
    for (usize i = 0; i < records_.size();) {
        Record& r = records_[i];
        bool present = false;
        for (const ChunkView& c : chunks) present = present || c.entity == r.chunk;
        if (present) {
            ++i;
            continue;
        }
        unload(world, r);
        records_.erase(i);
    }

    u32 budget = settle ? ~0u : math::max(config_.max_instantiations_per_pump, 1u);
    u32 in_flight = 0;
    for (const Record& r : records_) in_flight += r.state == ChunkState::Loading ? 1u : 0u;

    for (const ChunkView& c : chunks) {
        Record* r = find(c.entity);
        if (!r) {
            records_.push_back(Record{});
            r = &records_.back();
            r->chunk = c.entity;
            r->scene = c.chunk.scene.guid;
        }
        if (r->scene != c.chunk.scene.guid) {  // the game pointed the chunk at another scene
            unload(world, *r);
            r->scene = c.chunk.scene.guid;
        }
        f32 d = kFar;
        for (const math::vec3& p : focus) d = math::min(d, distance_to_box(p, c.centre, c.chunk.half_extents));
        const bool on = !c.chunk.disabled;
        const bool want = on && (c.chunk.pinned || d <= c.chunk.load_distance);
        const bool keep = on && (c.chunk.pinned || d <= math::max(c.chunk.unload_distance, c.chunk.load_distance));

        switch (r->state) {
            case ChunkState::Unloaded: {
                if (!want) break;
                const asset::AssetKey key = assets_->find(r->scene);
                if (key == asset::kInvalidAssetKey) {
                    fail(*r, Errc::NotFound);
                    break;
                }
                if (sync) {
                    if (budget == 0) break;  // next pump
                    --budget;
                    Result<asset::AssetHandle<asset::Scene>> h = assets_->load_sync<asset::Scene>(key);
                    if (!h) {
                        fail(*r, h.error().code);
                        break;
                    }
                    instantiate(world, *r, asset::AnyHandle(h.value()));
                } else {
                    if (in_flight >= math::max(config_.max_loads_in_flight, 1u)) break;
                    ++in_flight;
                    r->ticket = assets_->request_async<asset::Scene>(key);
                    r->order = next_order_++;
                    r->state = ChunkState::Loading;
                }
                break;
            }
            case ChunkState::Loading:
                if (!keep) cancel(*r);
                break;
            case ChunkState::Loaded:
                if (!keep) unload(world, *r);
                break;
            case ChunkState::Failed:
                if (!keep) r->state = ChunkState::Unloaded;  // retried when the focus comes back
                break;
        }
    }

    // Finished async loads, first-come first-served within the budget. Settle waits for them.
    if (settle) assets_->wait_async();
    else assets_->poll_async();
    for (;;) {
        Record* next = nullptr;
        for (Record& r : records_) {
            if (r.state != ChunkState::Loading) continue;
            const asset::AsyncStatus s = assets_->async_status(r.ticket);
            if (s == asset::AsyncStatus::Pending) continue;
            if (!next || r.order < next->order) next = &r;
        }
        if (!next) break;
        if (assets_->async_status(next->ticket) == asset::AsyncStatus::Ready && budget == 0) break;
        Result<asset::AnyHandle> h = assets_->take_async(next->ticket);
        if (!h) {
            fail(*next, h.error().code);
            continue;
        }
        --budget;
        instantiate(world, *next, h.value());
    }

    // Status for games and tools; written only when it changed (change detection stays quiet).
    stats_.chunks = static_cast<u32>(records_.size());
    stats_.loaded = stats_.loading = stats_.failed = stats_.streamed_entities = 0;
    for (const Record& r : records_) {
        u32 alive = 0;
        for (Entity e : r.spawned) alive += world.alive(e) ? 1u : 0u;
        stats_.loaded += r.state == ChunkState::Loaded ? 1u : 0u;
        stats_.loading += r.state == ChunkState::Loading ? 1u : 0u;
        stats_.failed += r.state == ChunkState::Failed ? 1u : 0u;
        stats_.streamed_entities += alive;
        SceneChunkStatus status;
        status.state = r.state;
        status.entity_count = r.state == ChunkState::Loaded ? alive : 0u;
        const SceneChunkStatus* cur = world.try_get<SceneChunkStatus>(r.chunk);
        if (!cur || cur->state != status.state || cur->entity_count != status.entity_count) world.set(r.chunk, status);
    }
}

void install_streaming(World& world, asset::AssetSystem& assets, StreamingConfig config) {
    if (SceneStreaming* s = world.try_resource<SceneStreaming>()) {
        s->set_assets(&assets);
        s->config() = config;
        return;
    }
    world.set_resource(SceneStreaming(&assets, config));
}

void pump_streaming(World& world, PumpMode mode) {
    if (SceneStreaming* s = world.try_resource<SceneStreaming>()) s->pump(world, mode);
}

}  // namespace mce::scene
