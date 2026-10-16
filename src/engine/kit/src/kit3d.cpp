#include "mce/kit/kit3d.h"

#include <algorithm>
#include <cmath>

#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/log.h"
#include "mce/string_id.h"
#include "kit3d_internal.h"

namespace mce::kit {

// World::state_hash hashes component bytes: these layouts have no implicit padding.
static_assert(sizeof(MeshRef) == 96);
static_assert(sizeof(PointLight3D) == 44);
static_assert(sizeof(SpotLight3D) == 64);
static_assert(sizeof(ReflectionProbe3D) == 64);
static_assert(sizeof(GiVolume3D) == 88);
static_assert(sizeof(Camera3DComponent) == 24);
static_assert(sizeof(Environment3D) == 440);
static_assert(sizeof(BoxCollider3D) == 40);
static_assert(sizeof(MeshCollider3D) == 32);
static_assert(sizeof(Foliage3D) == 120);
static_assert(sizeof(Decal3D) == 96);
static_assert(sizeof(ParticleEmitter3D) == 184);
static_assert(sizeof(Terrain3D) == 80);
static_assert(sizeof(TerrainCollider3D) == 8);
static_assert(sizeof(TerrainFoliage3D) == 128);

// The asset system mesh colliders read their meshes from (install_kit3d's argument).
struct Kit3DAssets {
    asset::AssetSystem* assets = nullptr;
};

void world_pose(World& w, Entity e, const LocalTransform& local, math::vec3& pos, math::quat& rot, math::vec3& scale) {
    if (const WorldTransform* wt = w.try_get<WorldTransform>(e)) {
        pos = wt->position;
        rot = wt->rotation;
        scale = wt->scale;
    } else {
        pos = local.position;
        rot = local.rotation;
        scale = local.scale;
    }
}

bool create_box_collider(World& w, Physics3D& phys, Entity e, const BoxCollider3D& c, const LocalTransform& t) {
    math::vec3 pos, scale;
    math::quat rot;
    world_pose(w, e, t, pos, rot, scale);
    physics::BodyDesc3D d;
    d.shape = physics::Shape3D::box({c.half.x * scale.x, c.half.y * scale.y, c.half.z * scale.z});
    d.rotation = rot * math::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, math::radians(c.pitch_deg));
    d.position = pos + math::rotate(rot, {c.offset.x * scale.x, c.offset.y * scale.y, c.offset.z * scale.z});
    d.friction = c.friction;
    d.layer = static_cast<u8>(c.layer & 15u);
    if (phys.world.create_body(e, d)) return true;
    MCE_LOG_WARN("kit3d: box collider on entity {} not created", e.index);
    return false;
}

bool create_mesh_collider(World& w, Physics3D& phys, asset::AssetSystem* assets, Entity e, const MeshCollider3D& c,
                          const LocalTransform& t) {
    if (!assets) {
        MCE_LOG_WARN("kit3d: mesh collider on entity {} skipped (no asset system)", e.index);
        return false;
    }
    Result<asset::AssetHandle<asset::Mesh>> h = assets->load_sync<asset::Mesh>(c.mesh.guid);
    if (!h) {
        MCE_LOG_WARN("kit3d: mesh collider on entity {}: mesh {} does not load", e.index, asset::to_string(c.mesh.guid).view());
        return false;
    }
    math::vec3 pos, scale;
    math::quat rot;
    world_pose(w, e, t, pos, rot, scale);
    Array<math::vec3> verts;
    Array<u32> indices;
    const bool read = mesh_triangles(assets->resolve(h.value()), math::scaling(scale), verts, indices);
    assets->release(asset::AnyHandle(h.value()));
    if (!read || indices.empty()) {
        MCE_LOG_WARN("kit3d: mesh collider on entity {}: no triangles", e.index);
        return false;
    }
    physics::BodyDesc3D d;
    d.shape = physics::Shape3D::mesh(verts.span(), indices.span());
    d.position = pos;
    d.rotation = rot;
    d.friction = c.friction;
    d.layer = static_cast<u8>(c.layer & 15u);
    if (phys.world.create_body(e, d)) return true;
    MCE_LOG_WARN("kit3d: mesh collider on entity {} not created", e.index);
    return false;
}

bool create_terrain_collider(World& w, Physics3D& phys, asset::AssetSystem* assets, Entity e, const TerrainCollider3D& c,
                             const LocalTransform& t) {
    const Terrain3D* terrain = w.try_get<Terrain3D>(e);
    if (!terrain || !assets) {
        MCE_LOG_WARN("kit3d: terrain collider on entity {} skipped ({})", e.index, terrain ? "no asset system" : "no Terrain3D");
        return false;
    }
    Result<asset::AssetHandle<asset::Terrain>> h = assets->load_sync<asset::Terrain>(terrain->terrain.guid);
    if (!h) {
        MCE_LOG_WARN("kit3d: terrain collider on entity {}: terrain {} does not load", e.index,
                     asset::to_string(terrain->terrain.guid).view());
        return false;
    }
    math::vec3 pos, scale;
    math::quat rot;
    world_pose(w, e, t, pos, rot, scale);
    Array<f32> heights;
    u32 n = 0;
    math::vec3 origin, spacing;
    if (const Result<asset::TerrainView> view = asset::TerrainView::read(assets->resolve(h.value()))) {
        const asset::TerrainView& v = view.value();
        n = v.resolution();
        heights.resize(usize(n) * n);
        for (u32 z = 0; z < n; ++z)
            for (u32 x = 0; x < n; ++x)
                heights[usize(z) * n + x] = v.hole(x, z) ? physics::kHeightFieldHole : v.sample(x, z);
        origin = {-0.5f * v.header->size[0] * scale.x, 0.0f, -0.5f * v.header->size[1] * scale.z};
        spacing = {v.spacing_x() * scale.x, scale.y, v.spacing_z() * scale.z};
    }
    assets->release(asset::AnyHandle(h.value()));
    if (n == 0) {
        MCE_LOG_WARN("kit3d: terrain collider on entity {}: unreadable terrain", e.index);
        return false;
    }
    physics::BodyDesc3D d;
    d.shape = physics::Shape3D::height_field(heights.span(), n, origin, spacing);
    d.position = pos;
    d.rotation = rot;
    d.friction = c.friction;
    d.layer = static_cast<u8>(c.layer & 15u);
    if (phys.world.create_body(e, d)) return true;
    MCE_LOG_WARN("kit3d: terrain collider on entity {} not created", e.index);
    return false;
}

asset::AssetSystem* kit3d_assets(World& w) {
    const Kit3DAssets* a = w.try_resource<Kit3DAssets>();
    return a ? a->assets : nullptr;
}

namespace {



void kit3d_colliders(Frame& f) {
    Physics3D& phys = f.resource<Physics3D>();
    asset::AssetSystem* assets = f.resource<Kit3DAssets>().assets;
    f.query<const BoxCollider3D&, const LocalTransform&>().each([&](Entity e, const BoxCollider3D& c, const LocalTransform& t) {
        if (create_box_collider(f.world(), phys, e, c, t)) ++phys.colliders;
    });
    f.query<const MeshCollider3D&, const LocalTransform&>().each([&](Entity e, const MeshCollider3D& c, const LocalTransform& t) {
        if (create_mesh_collider(f.world(), phys, assets, e, c, t)) ++phys.colliders;
    });
    f.query<const TerrainCollider3D&, const LocalTransform&>().each([&](Entity e, const TerrainCollider3D& c, const LocalTransform& t) {
        if (create_terrain_collider(f.world(), phys, assets, e, c, t)) ++phys.colliders;
    });
}

void kit3d_physics(Frame& f) {
    Physics3D& phys = f.resource<Physics3D>();
    if (phys.auto_step) phys.world.step(f.dt);
}

// The first authored Environment3D becomes the resource the game drives.
void kit3d_environment(Frame& f) {
    bool found = false;
    f.query<const Environment3D&>().each([&](const Environment3D& env) {
        if (found) return;
        found = true;
        f.resource<Environment3D>() = env;
    });
}

}  // namespace

u32 StaticInstances3D::mesh_slot(asset::Guid mesh) {
    if (mesh == asset::Guid{}) return ~0u;
    for (u32 i = 0; i < meshes.size(); ++i)
        if (meshes[i] == mesh) return i;
    meshes.push_back(mesh);
    return static_cast<u32>(meshes.size() - 1);
}

void StaticInstances3D::build_cells(f32 cell_size, f32 max_extent) {
    cells.clear();
    if (instances.empty()) return;
    const f32 inv = 1.0f / std::max(cell_size, 0.01f);
    auto key = [&](const StaticInstance3D& i) {
        const i64 cx = static_cast<i64>(std::floor(i.position.x * inv)) + (1 << 20);
        const i64 cz = static_cast<i64>(std::floor(i.position.z * inv)) + (1 << 20);
        return static_cast<u64>(cz) << 32 | static_cast<u64>(cx);
    };
    std::stable_sort(instances.begin(), instances.end(),
                     [&](const StaticInstance3D& a, const StaticInstance3D& b) { return key(a) < key(b); });
    for (u32 i = 0; i < instances.size();) {
        const u64 k = key(instances[i]);
        u32 j = i;
        math::vec3 lo = instances[i].position, hi = lo;
        f32 reach = 0.0f;
        while (j < instances.size() && key(instances[j]) == k) {
            const math::vec3 p = instances[j].position;
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            reach = std::max(reach, instances[j].scale);
            ++j;
        }
        StaticCell3D c;
        c.center = (lo + hi) * 0.5f;
        c.radius = math::length(hi - lo) * 0.5f + max_extent * std::max(reach, 1.0f);
        c.first = i;
        c.count = j - i;
        cells.push_back(c);
        i = j;
    }
}

bool mesh_triangles(Span<const byte> blob, const math::mat4& transform, Array<math::vec3>& vertices, Array<u32>& indices) {
    const Result<asset::MeshView> read = asset::MeshView::read(blob);
    if (!read) return false;
    const asset::MeshView& v = read.value();
    // Rest pose: parents precede children.
    Array<math::mat4> model;
    model.resize(v.nodes.size());
    for (u32 n = 0; n < v.nodes.size(); ++n) {
        const asset::MeshNode& node = v.nodes[n];
        const math::mat4 local = math::trs({node.translation[0], node.translation[1], node.translation[2]},
                                           {node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]},
                                           {node.scale[0], node.scale[1], node.scale[2]});
        model[n] = node.parent >= 0 ? model[static_cast<u32>(node.parent)] * local : local;
    }
    for (u32 s = 0; s < v.submeshes.size(); ++s) {
        const asset::MeshSubmesh& sm = v.submeshes[s];
        const bool skinned = v.skinned() && s < v.submesh_skins.size() && v.submesh_skins[s] >= 0;
        const math::mat4 place = transform * (skinned || sm.node >= model.size() ? math::mat4::identity() : model[sm.node]);
        const u32 base = static_cast<u32>(vertices.size());
        for (u32 i = 0; i < sm.vertex_count; ++i) {
            const asset::MeshVertex& q = v.vertices[sm.base_vertex + i];
            const math::vec3 p{sm.center[0] + static_cast<f32>(q.position[0]) / 32767.0f * sm.extent[0],
                               sm.center[1] + static_cast<f32>(q.position[1]) / 32767.0f * sm.extent[1],
                               sm.center[2] + static_cast<f32>(q.position[2]) / 32767.0f * sm.extent[2]};
            vertices.push_back(math::transform_point(place, p));
        }
        for (u32 i = 0; i < sm.index_count; ++i) indices.push_back(base + v.index(sm.first_index + i));
    }
    return true;
}

void install_kit3d(World& world, asset::AssetSystem* assets) {
    if (world.try_resource<Environment3D>()) {  // installed: a later call may bring the assets
        if (assets) world.resource<Kit3DAssets>().assets = assets;
        return;
    }
    world.register_components<MeshRef, PointLight3D, SpotLight3D, ReflectionProbe3D, GiVolume3D, Camera3DComponent, Animator,
                              Environment3D, BoxCollider3D, MeshCollider3D, Foliage3D, Decal3D, ParticleEmitter3D, Terrain3D,
                              TerrainCollider3D, TerrainFoliage3D>();
    world.set_resource(Environment3D{});
    world.set_resource(Kit3DAssets{assets});
    if (!world.try_resource<StaticInstances3D>()) world.set_resource(StaticInstances3D{});
    if (!world.try_resource<Physics3D>()) world.set_resource(Physics3D{});
    world.add_system(Phase::Startup, "kit3d_environment", &kit3d_environment);
    world.add_system(Phase::Startup, "kit3d_colliders", &kit3d_colliders, "kit3d_environment");
    world.add_system(Phase::FixedUpdate_Physics, "kit3d_physics", &kit3d_physics);
    install_anim3d(world);
    add_anim3d_systems(world);
}

bool has_camera_3d(World& world) { return world.query<const Camera3DComponent&>().count() > 0; }

}  // namespace mce::kit
