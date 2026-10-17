// The HLOD bake (M-OPENWORLD A3): a streamed area's static meshes merged into one proxy mesh. The extract side of
// ChunkLod3D lives in kit3d_extract.cpp.

#include "mce/kit/chunk_lod3d.h"

#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
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

bool bake_chunk_hlod(asset::AssetSystem& assets, asset::Guid scene, const vec3& origin, const HlodBakeSettings3D& settings,
                     Array<byte>& out, HlodBakeStats3D* stats) {
    out.clear();
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
    views.reserve(count);  // MergeSource points at these: no reallocation past here
    Array<render::MergeSource> sources;
    sources.reserve(count);

    scratch.query<const MeshRef&>().each([&](Entity e, const MeshRef& m) {
        ++st.meshes;
        mat4 world;
        if (const WorldTransform* w = scratch.try_get<WorldTransform>(e)) {
            world = w->matrix;
        } else if (const LocalTransform* t = scratch.try_get<LocalTransform>(e)) {
            world = math::trs(t->position, t->rotation, t->scale);
        } else {
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
        render::MergeSource src;
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
        views.push_back(v.value());
        src.mesh = &views.back();
        src.world = math::translation(-origin) * world;
        src.tint = m.tint;
        src.emissive_tint = m.emissive_tint;
        sources.push_back(src);
    });

    render::MergeStats ms;
    const bool ok = !sources.empty() && render::build_merged_mesh(sources.span(), out, &ms);
    for (const asset::AnyHandle& h : held) assets.release(h);
    st.merged = ms.sources;
    st.skipped += ms.skipped;
    st.submeshes = ms.submeshes;
    st.vertices = ms.vertices;
    st.triangles = ms.triangles;
    st.lod_levels = ms.lod_levels;
    st.bounds = ms.bounds;
    if (stats) *stats = st;
    if (!ok) out.clear();
    return ok;
}

}  // namespace mce::kit
