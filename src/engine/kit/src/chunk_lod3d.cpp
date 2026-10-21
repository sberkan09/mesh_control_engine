// The HLOD bake (M-OPENWORLD A3): a streamed area's static meshes merged into one proxy mesh. The extract side of
// ChunkLod3D lives in kit3d_extract.cpp.

#include "mce/kit/chunk_lod3d.h"

#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/containers/hash_map.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/log.h"
#include "mce/render/static_merge.h"
#include "mce/scene/scene.h"

namespace mce::kit {

using math::mat4;
using math::vec3;

bool collect_chunk_sources(asset::AssetSystem& assets, asset::Guid scene, const vec3& origin, const HlodBakeSettings3D& settings,
                           ChunkSourcesFn fn, void* user, HlodBakeStats3D* stats) {
    HlodBakeStats3D st;
    World scratch;
    Array<Entity> spawned;
    if (const Result<void> r = scene::load(scratch, assets, scene, &spawned); !r) {
        MCE_LOG_WARN("hlod: the area scene {} does not load", asset::to_string(scene).view());
        if (stats) *stats = st;
        return false;
    }

    // Every reference this bake takes, released before returning (the views point into the payloads).
    Array<asset::AnyHandle> held;
    usize count = 0;
    scratch.query<const MeshRef&>().each([&](const MeshRef&) { ++count; });
    Array<asset::MeshView> views;
    views.reserve(count);  // ChunkSource3D points at these: no reallocation past here
    Array<ChunkSource3D> sources;
    sources.reserve(count);
    HashMap<u32, u32> groups;  // root entity index -> group

    // The local matrix of an entity without a WorldTransform: its LocalTransform composed up the ChildOf chain.
    const auto local_world = [&scratch](Entity e, mat4& world) {
        world = mat4::identity();
        for (u32 depth = 0; !e.is_null() && depth < 256; ++depth) {
            const LocalTransform* t = scratch.try_get<LocalTransform>(e);
            if (!t) return depth > 0;
            world = math::trs(t->position, t->rotation, t->scale) * world;
            const ChildOf* p = scratch.try_get<ChildOf>(e);
            e = p ? p->parent : Entity::null;
        }
        return true;
    };

    scratch.query<const MeshRef&>().each([&](Entity e, const MeshRef& m) {
        ++st.meshes;
        mat4 world;
        if (const WorldTransform* w = scratch.try_get<WorldTransform>(e)) {
            world = w->matrix;
        } else if (!local_world(e, world)) {
            ++st.skipped;
            return;
        }
        if (!m.visible || m.mesh.guid.is_null()) {
            ++st.skipped;
            return;
        }
        const Result<asset::AssetHandle<asset::Mesh>> h = assets.load_sync<asset::Mesh>(m.mesh.guid);
        if (!h) {
            ++st.skipped;
            return;
        }
        held.push_back(h.value());
        const Result<asset::MeshView> v = asset::MeshView::read(assets.resolve(h.value()));
        if (!v || v.value().skinned()) {
            ++st.skipped;
            return;
        }
        const asset::MeshHeader& hd = *v.value().header;
        const vec3 extent{hd.bounds_max[0] - hd.bounds_min[0], hd.bounds_max[1] - hd.bounds_min[1], hd.bounds_max[2] - hd.bounds_min[2]};
        const auto axis = [&world](int i) { return math::length(vec3{world.c[i][0], world.c[i][1], world.c[i][2]}); };
        const f32 scale = math::max(axis(0), math::max(axis(1), axis(2)));
        if (math::length(extent) * scale < settings.min_size) {
            ++st.small;
            return;
        }
        ChunkSource3D src;
        if (!m.material.guid.is_null()) {  // a .mcmat on every submesh
            const Result<asset::AssetHandle<asset::Material>> mh = assets.load_sync<asset::Material>(m.material.guid);
            if (!mh) {
                ++st.skipped;
                return;
            }
            held.push_back(mh.value());
            const Result<const asset::MaterialChunk*> mc = asset::MaterialChunk::read(assets.resolve(mh.value()));
            if (!mc) {
                ++st.skipped;
                return;
            }
            src.material = mc.value();
        }
        Entity root = e;
        for (u32 depth = 0; depth < 256; ++depth) {
            const ChildOf* p = scratch.try_get<ChildOf>(root);
            if (!p || p->parent.is_null()) break;
            root = p->parent;
        }
        src.group = *groups.try_emplace(root.index, static_cast<u32>(groups.size())).first;
        views.push_back(v.value());
        src.mesh = &views.back();
        src.world = math::translation(-origin) * world;
        src.tint = m.tint;
        src.emissive_tint = m.emissive_tint;
        sources.push_back(src);
    });

    fn(sources.span(), user);
    for (const asset::AnyHandle& h : held) assets.release(h);
    if (stats) *stats = st;
    return true;
}

bool bake_chunk_hlod(asset::AssetSystem& assets, asset::Guid scene, const vec3& origin, const HlodBakeSettings3D& settings,
                     Array<byte>& out, HlodBakeStats3D* stats) {
    out.clear();
    struct Ctx {
        Array<byte>* out;
        render::MergeStats ms;
        bool ok = false;
    } ctx{&out, {}, false};
    HlodBakeStats3D st;
    const bool loaded = collect_chunk_sources(
        assets, scene, origin, settings,
        [](Span<const ChunkSource3D> sources, void* user) {
            Ctx& c = *static_cast<Ctx*>(user);
            Array<render::MergeSource> merge;
            merge.reserve(sources.size());
            for (const ChunkSource3D& s : sources) {
                render::MergeSource m;
                m.mesh = s.mesh;
                m.world = s.world;
                m.tint = s.tint;
                m.emissive_tint = s.emissive_tint;
                m.material = s.material;
                merge.push_back(m);
            }
            c.ok = !merge.empty() && render::build_merged_mesh(merge.span(), *c.out, &c.ms);
        },
        &ctx, &st);
    st.merged = ctx.ms.sources;
    st.skipped += ctx.ms.skipped;
    st.submeshes = ctx.ms.submeshes;
    st.vertices = ctx.ms.vertices;
    st.triangles = ctx.ms.triangles;
    st.lod_levels = ctx.ms.lod_levels;
    st.bounds = ctx.ms.bounds;
    if (stats) *stats = st;
    if (!loaded || !ctx.ok) out.clear();
    return loaded && ctx.ok;
}

}  // namespace mce::kit
