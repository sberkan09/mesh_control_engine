#include "mce/kit/kit3d.h"

#include <algorithm>
#include <cmath>

#include "mce/ecs/world.h"
#include "mce/string_id.h"

namespace mce::kit {

// World::state_hash hashes component bytes: these layouts have no implicit padding.
static_assert(sizeof(MeshRef) == 72);
static_assert(sizeof(PointLight3D) == 40);
static_assert(sizeof(SpotLight3D) == 60);
static_assert(sizeof(Camera3DComponent) == 16);
static_assert(sizeof(Animator) == 80);
static_assert(sizeof(Environment3D) == 208);

namespace {

// The first authored Environment3D becomes the resource the game drives.
void kit3d_environment(Frame& f) {
    bool found = false;
    f.query<const Environment3D&>().each([&](const Environment3D& env) {
        if (found) return;
        found = true;
        f.resource<Environment3D>() = env;
    });
}

// Animator: time advances by dt * speed; a new clip name starts at 0 and cross-fades from the old.
void kit3d_animators(Frame& f) {
    const f32 dt = f.dt;
    f.query<Animator&>().each([&](Animator& a) {
        const u64 want = a.clip.empty() ? 0 : StringId(a.clip.view()).value;
        if (want != a.playing) {
            a.previous = a.playing;
            a.previous_time = a.time;
            a.playing = want;
            a.time = 0.0f;
            a.blend = (a.fade > 0.0f && a.previous != 0) ? 0.0f : 1.0f;
            if (a.blend >= 1.0f) a.previous = 0;
            return;  // the switching tick shows the clips as they were: both at their start
        }
        a.time += dt * a.speed;
        if (a.blend < 1.0f) {
            a.previous_time += dt * a.speed;
            a.blend = a.fade > 0.0f ? std::min(1.0f, a.blend + dt / a.fade) : 1.0f;
        }
        if (a.blend >= 1.0f) a.previous = 0;
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

void install_kit3d(World& world, asset::AssetSystem* /*assets*/) {
    if (world.try_resource<Environment3D>()) return;  // installed
    world.register_components<MeshRef, PointLight3D, SpotLight3D, Camera3DComponent, Animator, Environment3D>();
    world.set_resource(Environment3D{});
    if (!world.try_resource<StaticInstances3D>()) world.set_resource(StaticInstances3D{});
    world.add_system(Phase::Startup, "kit3d_environment", &kit3d_environment);
    world.add_system(Phase::FixedUpdate_Post, "kit3d_animators", &kit3d_animators);
}

bool has_camera_3d(World& world) { return world.query<const Camera3DComponent&>().count() > 0; }

}  // namespace mce::kit
