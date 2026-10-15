// The Animator system (clips, cross-fades, one-shots, AnimGraph state machines and their events)
// and AttachToBone (M-DATA-REACH 7). Clip lengths and bone poses come from a CPU-only copy of
// the mesh (a device-less render::MeshStore), loaded through install_kit3d's asset system for
// the meshes an Animator or an AttachToBone target uses.

#include <cmath>

#include "kit3d_internal.h"
#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/containers/array.h"
#include "mce/ecs/world.h"
#include "mce/kit/anim3d.h"
#include "mce/log.h"
#include "mce/render/mesh.h"
#include "mce/string_id.h"

namespace mce::kit {

// World::state_hash hashes component bytes: these layouts have no implicit padding.
static_assert(sizeof(Animator) == 216);
static_assert(sizeof(AttachToBone) == 80);
static_assert(sizeof(AnimEvent3D) == 16);

namespace {

using math::mat4;
using math::quat;
using math::vec3;

template <typename T>
bool load(asset::AssetSystem* assets, asset::Guid guid, asset::AnyHandle& handle, bool& failed, const char* what) {
    if (handle.generation != 0 && assets->is_alive(handle)) return true;
    if (failed) return false;
    if (handle.generation != 0) assets->release(handle);
    handle = {};
    Result<asset::AssetHandle<T>> h = assets->load_sync<T>(guid);
    if (!h) {
        MCE_LOG_WARN("kit3d: {} {} cannot be loaded", what, asset::to_string(guid).view());
        failed = true;
        return false;
    }
    handle = asset::AnyHandle(h.value());
    return true;
}

// The meshes and graphs the animation systems read, one reference per GUID (reloaded when the
// asset hot-reloads: the handle dies and the next lookup loads the new generation).
struct Anim3DAssets {
    struct MeshEntry {
        asset::Guid guid;
        asset::AnyHandle handle;
        u32 key = 0;
        u32 generation = 0;
        bool failed = false;
    };
    struct GraphEntry {
        asset::Guid guid;
        asset::AnyHandle handle;
        bool failed = false;
    };
    mem::UniquePtr<render::MeshStore> store;
    Array<MeshEntry> meshes;
    Array<GraphEntry> graphs;

    const render::GpuMesh* mesh(asset::AssetSystem* assets, asset::Guid guid) {
        if (!assets || guid == asset::Guid{}) return nullptr;
        MeshEntry* e = nullptr;
        for (MeshEntry& m : meshes)
            if (m.guid == guid) e = &m;
        if (!e) {
            MeshEntry fresh;
            fresh.guid = guid;
            fresh.key = static_cast<u32>(meshes.size());
            meshes.push_back(fresh);
            e = &meshes.back();
        }
        const u32 had = e->handle.generation;
        if (!load<asset::Mesh>(assets, guid, e->handle, e->failed, "animated mesh")) return nullptr;
        if (e->handle.generation != had || !store->get(e->key)) {
            const Span<const byte> blob = assets->resolve(asset::AssetHandle<asset::Mesh>{e->handle.index, e->handle.generation});
            if (!store->upload(e->key, ++e->generation, blob)) {
                MCE_LOG_WARN("kit3d: animated mesh {} does not read", asset::to_string(guid).view());
                e->failed = true;
                return nullptr;
            }
        }
        return store->get(e->key);
    }

    bool graph(asset::AssetSystem* assets, asset::Guid guid, asset::AnimGraphView& out) {
        if (!assets || guid == asset::Guid{}) return false;
        GraphEntry* e = nullptr;
        for (GraphEntry& g : graphs)
            if (g.guid == guid) e = &g;
        if (!e) {
            graphs.push_back(GraphEntry{guid, {}, false});
            e = &graphs.back();
        }
        if (!load<asset::AnimGraph>(assets, guid, e->handle, e->failed, "animation graph")) return false;
        Result<asset::AnimGraphView> v =
            asset::AnimGraphView::read(assets->resolve(asset::AssetHandle<asset::AnimGraph>{e->handle.index, e->handle.generation}));
        if (!v) return false;
        out = v.value();
        return true;
    }
};


f32 clip_duration(const render::GpuMesh* mesh, u64 id) {
    if (!mesh || id == 0) return 0.0f;
    for (const render::GpuMeshClip& c : mesh->clips)
        if (StringId(c.name.view()).value == id) return c.duration;
    return 0.0f;
}

u64 clip_id(const char* name, usize cap) {
    const StringView s = asset::AnimGraphView::text(name, cap);
    return s.empty() ? 0 : StringId(s).value;
}

bool holds(const asset::AnimGraphCondition& c, const math::vec4& params) {
    const f32 p = c.param == 0 ? params.x : c.param == 1 ? params.y : c.param == 2 ? params.z : params.w;
    switch (c.op) {
        case asset::AnimCompare::Greater: return p > c.value;
        case asset::AnimCompare::Less: return p < c.value;
        case asset::AnimCompare::GreaterEqual: return p >= c.value;
        case asset::AnimCompare::LessEqual: return p <= c.value;
        case asset::AnimCompare::Equal: return p == c.value;
        case asset::AnimCompare::NotEqual: return p != c.value;
    }
    return false;
}

f32 param_of(const math::vec4& params, u32 i) { return i == 0 ? params.x : i == 1 ? params.y : i == 2 ? params.z : params.w; }

// Starts `clip` at time 0, cross-fading from what plays over `fade` seconds.
void start_clip(Animator& a, u64 clip, f32 fade) {
    a.previous = a.playing;
    a.previous_time = a.time;
    a.playing = clip;
    a.time = 0.0f;
    a.finished = false;
    a.blend = (fade > 0.0f && a.previous != 0 && a.previous != clip) ? 0.0f : 1.0f;
    if (a.blend >= 1.0f) a.previous = 0;
}

// The blend state's two neighbouring clips and the weight of the second at the parameter value.
void pick_blend(const asset::AnimGraphView& g, const asset::AnimGraphState& s, const math::vec4& params, u64& first, u64& second,
                f32& weight) {
    const f32 p = param_of(params, s.blend_param);
    const Span<const asset::AnimGraphBlendPoint> pts = g.blend_points.subspan(s.first_blend, s.blend_count);
    u32 i = 0;
    while (i + 1 < pts.size() && p > pts[i + 1].at) ++i;
    first = clip_id(pts[i].clip, sizeof pts[i].clip);
    if (i + 1 >= pts.size() || p <= pts[i].at) {
        second = 0;
        weight = 0.0f;
        return;
    }
    second = clip_id(pts[i + 1].clip, sizeof pts[i + 1].clip);
    const f32 span = pts[i + 1].at - pts[i].at;
    weight = span > 0.0f ? std::fmin(1.0f, (p - pts[i].at) / span) : 1.0f;
}

void enter_state(Animator& a, const asset::AnimGraphView& g, u32 index, f32 fade) {
    const asset::AnimGraphState& s = g.states[index];
    a.state = asset::AnimGraphView::text(s.name, sizeof s.name);
    u64 first = clip_id(s.clip, sizeof s.clip), second = 0;
    f32 weight = 0.0f;
    if (s.blend_count > 0) pick_blend(g, s, a.params, first, second, weight);
    start_clip(a, first, fade);
    a.blend_clip = second;
    a.blend_weight = weight;
    const StringView name = first != 0 && s.blend_count == 0 ? asset::AnimGraphView::text(s.clip, sizeof s.clip) : StringView();
    a.clip = name;  // a blend state leaves `clip` empty: `playing` / `blend_clip` name its clips
}

// Returns true when it entered a state this tick.
bool run_graph(Frame& f, Entity e, Animator& a, const asset::AnimGraphView& g, const render::GpuMesh* mesh) {
    if (g.states.empty()) return false;
    u32 cur = g.find_state(a.state.view());
    if (a.restart || cur >= g.states.size()) {
        a.restart = false;
        const u32 start = g.header->start_state < g.states.size() ? g.header->start_state : 0;
        enter_state(a, g, start, cur >= g.states.size() ? 0.0f : a.fade);
        return true;  // the entering tick shows the state at its start
    }
    const asset::AnimGraphState& s = g.states[cur];
    if (s.blend_count > 0) {
        u64 first = 0, second = 0;
        pick_blend(g, s, a.params, first, second, a.blend_weight);
        if (first != a.playing) {  // the parameter crossed a point: keep the phase on the new pair
            const f32 from = clip_duration(mesh, a.playing), to = clip_duration(mesh, first);
            a.playing = first;
            if (from > 0.0f && to > 0.0f) a.time = a.time / from * to;
        }
        a.blend_clip = second;
    }

    // Advance, firing the state's events the playhead crosses ([t0, t1) per pass).
    const f32 duration = clip_duration(mesh, a.playing);
    const f32 t0 = a.time;
    f32 t1 = t0 + f.dt * a.speed * s.speed;
    auto fire_between = [&](f32 lo, f32 hi, bool closed) {
        for (u32 i = 0; i < s.event_count; ++i) {
            const asset::AnimGraphEvent& ev = g.events[s.first_event + i];
            if (ev.time >= lo && (ev.time < hi || (closed && ev.time <= hi))) {
                ++a.events_fired;
                a.last_event = asset::AnimGraphView::text(ev.name, sizeof ev.name);
                f.events().push(AnimEvent3D{e, ev.id});
            }
        }
    };
    if (!a.finished && t1 > t0) {
        if (duration > 0.0f && !s.loop && t1 >= duration) {
            fire_between(t0, duration, true);
            t1 = std::nextafter(duration, 0.0f);  // poses sample the clip looping: hold just under its end
            a.finished = true;
        } else if (duration > 0.0f && s.loop && t1 >= duration) {
            fire_between(t0, duration, false);
            t1 = std::fmod(t1, duration);
            fire_between(0.0f, t1, false);
        } else {
            fire_between(t0, t1, false);
        }
        a.time = t1;
    }

    // The first transition out of the state (or from any state) whose conditions all hold.
    for (const asset::AnimGraphTransition& tr : g.transitions) {
        if (tr.from != cur && !(tr.from == asset::kAnimAnyState && tr.to != cur)) continue;
        if (tr.on_finish && !a.finished) continue;
        bool ok = true;
        for (u32 i = 0; i < tr.condition_count && ok; ++i) ok = holds(g.conditions[tr.first_condition + i], a.params);
        if (!ok) continue;
        enter_state(a, g, tr.to, tr.fade);
        a.fade = tr.fade;
        return true;
    }
    return false;
}

// Animator: time advances by dt * speed; a new clip name starts at 0 and cross-fades from the old;
// a graph picks the clip.
void kit3d_animators(Frame& f) {
    const f32 dt = f.dt;
    World& w = f.world();
    asset::AssetSystem* assets = kit3d_assets(w);
    Anim3DAssets& cache = w.resource<Anim3DAssets>();
    f.query<Animator&>().each([&](Entity e, Animator& a) {
        const MeshRef* ref = w.try_get<MeshRef>(e);
        const render::GpuMesh* mesh = ref ? cache.mesh(assets, ref->mesh.guid) : nullptr;
        if (a.graph.guid != asset::Guid{}) {
            asset::AnimGraphView g;
            if (cache.graph(assets, a.graph.guid, g)) {
                if (!run_graph(f, e, a, g, mesh) && a.blend < 1.0f) {
                    a.previous_time += dt * a.speed;
                    a.blend = a.fade > 0.0f ? std::fmin(1.0f, a.blend + dt / a.fade) : 1.0f;
                }
                if (a.blend >= 1.0f) a.previous = 0;
                return;
            }
        }
        a.blend_clip = 0;
        a.blend_weight = 0.0f;
        const u64 want = a.clip.empty() ? 0 : StringId(a.clip.view()).value;
        if (want != a.playing || a.restart) {
            a.restart = false;
            start_clip(a, want, a.fade);
            return;  // the switching tick shows the clips as they were: both at their start
        }
        if (!a.finished) a.time += dt * a.speed;
        const f32 duration = clip_duration(mesh, a.playing);
        if (duration > 0.0f && a.time >= duration) {
            if (a.loop) {
                a.time = std::fmod(a.time, duration);
            } else {
                a.time = std::nextafter(duration, 0.0f);
                a.finished = true;
            }
        }
        if (a.blend < 1.0f) {
            a.previous_time += dt * a.speed;
            a.blend = a.fade > 0.0f ? std::fmin(1.0f, a.blend + dt / a.fade) : 1.0f;
        }
        if (a.blend >= 1.0f) a.previous = 0;
    });
}

u32 find_clip(const render::GpuMesh& mesh, u64 id) {
    for (u32 i = 0; i < mesh.clips.size(); ++i)
        if (StringId(mesh.clips[i].name.view()).value == id) return i;
    return ~0u;
}

// The model-space matrix of node `node` of `target`'s mesh: its ragdoll's pose blended over the
// Animator's (the rest pose without either).
mat4 bone_pose(World& w, Entity target, const render::GpuMesh& mesh, u32 node) {
    const Animator* a = w.try_get<Animator>(target);
    const Ragdoll3DPoses* rp = w.try_resource<Ragdoll3DPoses>();
    if (!(rp && rp->poses.find(target)) && (!a || a->playing == 0)) return mesh.rest_pose[node];
    Array<mat4> pose;
    pose.resize(mesh.nodes.size());
    animated_pose(mesh, a, pose.span());
    apply_ragdoll_pose(w, target, pose.span());
    return pose[node];
}

void kit3d_attach(Frame& f) {
    World& w = f.world();
    asset::AssetSystem* assets = kit3d_assets(w);
    Anim3DAssets& cache = w.resource<Anim3DAssets>();
    f.query<AttachToBone&, LocalTransform&>().each([&](AttachToBone& at, LocalTransform& t) {
        at.found = 0;
        const MeshRef* ref = w.alive(at.target) ? w.try_get<MeshRef>(at.target) : nullptr;
        const LocalTransform* tt = ref ? w.try_get<LocalTransform>(at.target) : nullptr;
        if (!tt) return;
        const render::GpuMesh* mesh = cache.mesh(assets, ref->mesh.guid);
        if (!mesh) return;
        const u32 node = mesh->find_node(at.bone.view());
        if (node >= mesh->nodes.size()) return;
        vec3 pos, scale;
        quat rot;
        world_pose(w, at.target, *tt, pos, rot, scale);
        const mat4 m = math::trs(pos, rot, scale) * bone_pose(w, at.target, *mesh, node) *
                       math::trs(at.offset, math::normalize(at.rotation), {1.0f, 1.0f, 1.0f});
        const vec3 c0 = m.col(0).xyz(), c1 = m.col(1).xyz(), c2 = m.col(2).xyz();
        t.position = m.col(3).xyz();
        t.scale = {math::length(c0), math::length(c1), math::length(c2)};
        t.rotation = quat_from_basis(math::normalize(c0), math::normalize(c1), math::normalize(c2));
        at.found = 1;
    });
}

}  // namespace

// Shepperd's method.
quat quat_from_basis(vec3 c0, vec3 c1, vec3 c2) {
    const f32 trace = c0.x + c1.y + c2.z;
    quat q;
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(c1.z - c2.y) / s, (c2.x - c0.z) / s, (c0.y - c1.x) / s, 0.25f * s};
    } else if (c0.x > c1.y && c0.x > c2.z) {
        const f32 s = std::sqrt(1.0f + c0.x - c1.y - c2.z) * 2.0f;
        q = {0.25f * s, (c1.x + c0.y) / s, (c2.x + c0.z) / s, (c1.z - c2.y) / s};
    } else if (c1.y > c2.z) {
        const f32 s = std::sqrt(1.0f + c1.y - c0.x - c2.z) * 2.0f;
        q = {(c1.x + c0.y) / s, 0.25f * s, (c2.y + c1.z) / s, (c2.x - c0.z) / s};
    } else {
        const f32 s = std::sqrt(1.0f + c2.z - c0.x - c1.y) * 2.0f;
        q = {(c2.x + c0.z) / s, (c2.y + c1.z) / s, 0.25f * s, (c0.y - c1.x) / s};
    }
    return math::normalize(q);
}

const render::GpuMesh* anim3d_mesh(World& w, asset::Guid guid) {
    Anim3DAssets* cache = w.try_resource<Anim3DAssets>();
    return cache ? cache->mesh(kit3d_assets(w), guid) : nullptr;
}

void animated_pose(const render::GpuMesh& mesh, const Animator* a, Span<mat4> out) {
    render::ClipSample samples[3];
    const usize n = a && a->playing != 0 ? anim_samples(*a, mesh, [&](u64 id) { return find_clip(mesh, id); }, samples) : 0;
    if (n > 0) {
        pose_samples(mesh, samples, n, out);
        return;
    }
    for (usize i = 0; i < out.size() && i < mesh.rest_pose.size(); ++i) out[i] = mesh.rest_pose[i];
}

void install_anim3d(World& world) {
    world.register_components<AttachToBone>();
    world.register_event<AnimEvent3D>(Phase::FixedUpdate_Post);
    Anim3DAssets a;
    a.store = mem::make_unique<render::MeshStore>(mem::heap());
    a.store->init(nullptr);
    world.set_resource(static_cast<Anim3DAssets&&>(a));
}

void add_anim3d_systems(World& world) {
    world.add_system(Phase::FixedUpdate_Post, "kit3d_animators", &kit3d_animators);
    world.add_system(Phase::FixedUpdate_Post, "kit3d_attach", &kit3d_attach, "kit3d_animators");
}

}  // namespace mce::kit
