// The Animator system (clips, cross-fades, one-shots, AnimGraph state machines and their events)
// and AttachToBone (M-DATA-REACH 7). Clip lengths and bone poses come from a CPU-only copy of
// the mesh (a device-less render::MeshStore), loaded through install_kit3d's asset system for
// the meshes an Animator or an AttachToBone target uses.
//
// M-GAMEPLAY3D step 5: 2D blend spaces, layers (bone masks, additive clips), root motion and IK
// (two-bone, look-at, foot placement). The pose of a graph with those extras is built in local
// TRS (render::sample_local), layered, root-locked, composed and then bent by the IK goals that
// "kit3d_ik" gathers each tick into the AnimIK3D resource.

#include <cmath>
#include <optional>

#include "kit3d_internal.h"
#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/containers/array.h"
#include "mce/ecs/world.h"
#include "mce/kit/anim3d.h"
#include "mce/kit/bodies3d.h"
#include "mce/log.h"
#include "mce/render/mesh.h"
#include "mce/string_id.h"

namespace mce::kit {

// World::state_hash hashes component bytes: these layouts have no implicit padding.
static_assert(sizeof(Animator) == 248);
static_assert(sizeof(AttachToBone) == 80);
static_assert(sizeof(IKTarget3D) == 48);
static_assert(sizeof(AnimEvent3D) == 16);

namespace {

using math::mat4;
using math::quat;
using math::vec3;
using math::vec4;
using render::ClipSample;
using render::GpuMeshNode;

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

// Gradient-band weights (freeform cartesian) of a 2D blend state's points at the parameters:
// a point has full weight on itself and none past any other point; they sum to 1.
void blend2d_weights(const asset::AnimGraphView& g, const asset::AnimGraphState& s, const math::vec4& params,
                     f32 (&w)[asset::kAnimGraphMaxBlend2D]) {
    const Span<const asset::AnimGraphBlendPoint2D> pts = g.blend2d.subspan(s.first_blend, s.blend_count);
    const f32 px = param_of(params, s.blend_param), py = param_of(params, s.blend_param_y);
    f32 total = 0.0f;
    for (u32 i = 0; i < pts.size(); ++i) {
        f32 h = 1.0f;
        for (u32 j = 0; j < pts.size(); ++j) {
            const f32 vx = pts[j].x - pts[i].x, vy = pts[j].y - pts[i].y, len2 = vx * vx + vy * vy;
            if (j == i || !(len2 > 0.0f)) continue;
            h = math::min(h, 1.0f - ((px - pts[i].x) * vx + (py - pts[i].y) * vy) / len2);
        }
        w[i] = math::max(h, 0.0f);
        total += w[i];
    }
    if (total > 0.0f) {
        for (u32 i = 0; i < pts.size(); ++i) w[i] /= total;
        return;
    }
    u32 best = 0;  // outside every band (cannot happen with 3+ points, kept for safety): the nearest point
    f32 best_d = 0.0f;
    for (u32 i = 0; i < pts.size(); ++i) {
        const f32 d = (px - pts[i].x) * (px - pts[i].x) + (py - pts[i].y) * (py - pts[i].y);
        if (i == 0 || d < best_d) best = i, best_d = d;
        w[i] = 0.0f;
    }
    w[best] = 1.0f;
}

// A 2D blend state's heaviest clip (the state's timeline) and the next heaviest with its weight.
void pick_blend2d(const asset::AnimGraphView& g, const asset::AnimGraphState& s, const math::vec4& params, u64& first, u64& second,
                  f32& weight) {
    f32 w[asset::kAnimGraphMaxBlend2D];
    blend2d_weights(g, s, params, w);
    u32 a = 0, b = ~0u;
    for (u32 i = 1; i < s.blend_count; ++i)
        if (w[i] > w[a]) a = i;
    for (u32 i = 0; i < s.blend_count; ++i)
        if (i != a && w[i] > 0.0f && (b == ~0u || w[i] > w[b])) b = i;
    const asset::AnimGraphBlendPoint2D* pts = g.blend2d.data() + s.first_blend;
    first = clip_id(pts[a].clip, sizeof pts[a].clip);
    second = b == ~0u ? 0 : clip_id(pts[b].clip, sizeof pts[b].clip);
    weight = b == ~0u ? 0.0f : w[b];
}

void pick_state_blend(const asset::AnimGraphView& g, const asset::AnimGraphState& s, const math::vec4& params, u64& first,
                      u64& second, f32& weight) {
    if (s.blend_2d) pick_blend2d(g, s, params, first, second, weight);
    else pick_blend(g, s, params, first, second, weight);
}

void enter_state(Animator& a, const asset::AnimGraphView& g, u32 index, f32 fade) {
    const asset::AnimGraphState& s = g.states[index];
    a.state = asset::AnimGraphView::text(s.name, sizeof s.name);
    u64 first = clip_id(s.clip, sizeof s.clip), second = 0;
    f32 weight = 0.0f;
    if (s.blend_count > 0) pick_state_blend(g, s, a.params, first, second, weight);
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
        pick_state_blend(g, s, a.params, first, second, a.blend_weight);
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

u32 find_clip(const render::GpuMesh& mesh, u64 id) {
    for (u32 i = 0; i < mesh.clips.size(); ++i)
        if (StringId(mesh.clips[i].name.view()).value == id) return i;
    return ~0u;
}

f32 vec_of(const vec4& v, u32 i) { return i == 0 ? v.x : i == 1 ? v.y : i == 2 ? v.z : v.w; }
void set_vec(vec4& v, u32 i, f32 x) { (i == 0 ? v.x : i == 1 ? v.y : i == 2 ? v.z : v.w) = x; }

// A layer's or IK chain's weight: its own, times its parameter (clamped to 0..1) when it has one.
f32 weight_of(f32 weight, u8 param, const vec4& params) {
    return param == asset::kAnimNoParam ? weight : weight * math::clamp(vec_of(params, param), 0.0f, 1.0f);
}

// The clips of the state playing (not the one fading out) at time `t` of `a.playing`, weights
// summing to 1: one clip, a 1D blend's pair, or a 2D blend's points at the same phase.
usize state_samples(const Animator& a, const render::GpuMesh& mesh, const asset::AnimGraphView& g, f32 t,
                    ClipSample (&out)[asset::kAnimGraphMaxBlend2D]) {
    const u32 clip = a.playing != 0 ? find_clip(mesh, a.playing) : ~0u;
    if (clip == ~0u) return 0;
    const f32 from = mesh.clips[clip].duration;
    const auto scaled = [&](u32 c) {
        const f32 to = mesh.clips[c].duration;
        return from > 0.0f ? t / from * to : t;
    };
    const u32 si = g.find_state(a.state.view());
    if (si < g.states.size() && g.states[si].blend_2d) {
        const asset::AnimGraphState& s = g.states[si];
        f32 w[asset::kAnimGraphMaxBlend2D];
        blend2d_weights(g, s, a.params, w);
        usize n = 0;
        for (u32 i = 0; i < s.blend_count; ++i) {
            const asset::AnimGraphBlendPoint2D& p = g.blend2d[s.first_blend + i];
            const u32 c = w[i] > 0.0f ? find_clip(mesh, clip_id(p.clip, sizeof p.clip)) : ~0u;
            if (c != ~0u) out[n++] = {c, scaled(c), w[i]};
        }
        if (n > 0) return n;
    }
    const u32 second = a.blend_clip != 0 && a.blend_weight > 0.0f ? find_clip(mesh, a.blend_clip) : ~0u;
    if (second == ~0u) {
        out[0] = {clip, t, 1.0f};
        return 1;
    }
    out[0] = {clip, t, 1.0f - a.blend_weight};
    out[1] = {second, scaled(second), a.blend_weight};
    return 2;
}

// ---- root motion ----

// The root motion bone and its parent's rest frame (the bone's ground motion is measured in the
// mesh's model space through it).
struct RootBone {
    u32 node = ~0u;
    mat4 parent = mat4::identity();
    quat parent_rot;
};

quat rotation_of(const mat4& m) {
    return quat_from_basis(math::normalize(m.col(0).xyz()), math::normalize(m.col(1).xyz()), math::normalize(m.col(2).xyz()));
}

bool root_bone(const render::GpuMesh& mesh, const asset::AnimGraphView& g, RootBone& out) {
    const StringView name = g.root_bone();
    if (name.empty()) return false;
    out.node = mesh.find_node(name);
    if (out.node >= mesh.nodes.size()) return false;
    const i32 parent = mesh.nodes[out.node].parent;
    if (parent >= 0) {
        out.parent = mesh.rest_pose[static_cast<usize>(parent)];
        out.parent_rot = rotation_of(out.parent);
    }
    return true;
}

// The turn about model +Y that takes rotation `from` to `to` (radians, -pi..pi).
f32 yaw_between(quat from, quat to) {
    const quat d = to * math::conjugate(from);
    f32 y = 2.0f * std::atan2(d.y, d.w);
    if (y > math::kPi) y -= 2.0f * math::kPi;
    else if (y < -math::kPi) y += 2.0f * math::kPi;
    return y;
}

// The root bone of clip `c` at `t`: its model-space position and rotation.
void root_at(const render::GpuMesh& mesh, const RootBone& r, u32 c, f32 t, vec3& pos, quat& rot) {
    const GpuMeshNode n = render::sample_node(mesh, c, t, r.node);
    pos = (r.parent * vec4{n.translation.x, n.translation.y, n.translation.z, 1.0f}).xyz();
    rot = r.parent_rot * n.rotation;
}

// Ground motion (x, z) and turn of the root from clip time t0 to t1, across the loop when `wrapped`.
void root_delta(const render::GpuMesh& mesh, const RootBone& r, u32 c, f32 t0, f32 t1, bool wrapped, vec3& move, f32& turn) {
    vec3 p0, p1;
    quat q0, q1;
    root_at(mesh, r, c, t0, p0, q0);
    if (wrapped) {
        const f32 end = std::nextafter(mesh.clips[c].duration, 0.0f);
        vec3 pe, ps;
        quat qe, qs;
        root_at(mesh, r, c, end, pe, qe);
        root_at(mesh, r, c, 0.0f, ps, qs);
        root_at(mesh, r, c, t1, p1, q1);
        move = (pe - p0) + (p1 - ps);
        turn = yaw_between(q0, qe) + yaw_between(qs, q1);
    } else {
        root_at(mesh, r, c, t1, p1, q1);
        move = p1 - p0;
        turn = yaw_between(q0, q1);
    }
    move.y = 0.0f;
}

// Moves the entity by the root's motion over the tick that took the state's playhead from t0 to
// a.time (and reports it in root_velocity / root_turn).
void apply_root_motion(World& w, Entity e, Animator& a, const render::GpuMesh& mesh, const asset::AnimGraphView& g, f32 t0, f32 dt) {
    RootBone r;
    if (!(dt > 0.0f) || !root_bone(mesh, g, r)) return;
    ClipSample from[asset::kAnimGraphMaxBlend2D], to[asset::kAnimGraphMaxBlend2D];
    const usize n = state_samples(a, mesh, g, t0, from);
    if (state_samples(a, mesh, g, a.time, to) != n || n == 0) return;
    const bool wrapped = a.time < t0;
    vec3 move;
    f32 turn = 0.0f;
    for (usize i = 0; i < n; ++i) {
        vec3 m;
        f32 y = 0.0f;
        root_delta(mesh, r, from[i].clip, from[i].time, to[i].time, wrapped, m, y);
        move += m * to[i].weight;
        turn += y * to[i].weight;
    }
    LocalTransform* t = w.try_get_mut<LocalTransform>(e);
    if (!t) return;
    vec3 pos, scale;
    quat rot;
    world_pose(w, e, *t, pos, rot, scale);
    const vec3 v = math::rotate(rot, vec3{move.x * scale.x, 0.0f, move.z * scale.z}) * (1.0f / dt);
    a.root_velocity = v;
    a.root_turn = turn / dt;
    if (a.root_motion != 1) return;
    CharacterController3D* cc = w.try_get_mut<CharacterController3D>(e);
    if (cc && !cc->use_input) cc->move = {v.x, 0.0f, v.z};
    else if (!cc) t->position += v * dt;
    if (turn != 0.0f) t->rotation = math::normalize(math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, turn) * t->rotation);
}

// Advances each layer's playhead.
void advance_layers(Animator& a, const render::GpuMesh* mesh, const asset::AnimGraphView& g, f32 dt) {
    for (u32 i = 0; i < g.layers.size() && i < asset::kAnimGraphMaxLayers; ++i) {
        const asset::AnimGraphLayer& l = g.layers[i];
        const f32 duration = clip_duration(mesh, clip_id(l.clip, sizeof l.clip));
        f32 t = a.restart ? 0.0f : vec_of(a.layer_time, i) + dt * a.speed * l.speed;
        if (duration > 0.0f && t >= duration) t = l.loop ? std::fmod(t, duration) : std::nextafter(duration, 0.0f);
        set_vec(a.layer_time, i, math::max(t, 0.0f));
    }
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
                if (g.extension) advance_layers(a, mesh, g, dt);  // before run_graph clears `restart`
                const f32 t0 = a.time;
                const u64 playing = a.playing;
                const bool entered = run_graph(f, e, a, g, mesh);
                if (!entered && a.blend < 1.0f) {
                    a.previous_time += dt * a.speed;
                    a.blend = a.fade > 0.0f ? std::fmin(1.0f, a.blend + dt / a.fade) : 1.0f;
                }
                if (a.blend >= 1.0f) a.previous = 0;
                a.root_velocity = {};
                a.root_turn = 0.0f;
                if (g.extension && mesh && !entered && a.playing == playing && a.root_motion != 0)
                    apply_root_motion(w, e, a, *mesh, g, t0, dt);
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

// The model-space matrix of node `node` of `target`'s mesh: its ragdoll's pose blended over the
// Animator's (the rest pose without either).
mat4 bone_pose(World& w, Entity target, const render::GpuMesh& mesh, u32 node) {
    const Animator* a = w.try_get<Animator>(target);
    const Ragdoll3DPoses* rp = w.try_resource<Ragdoll3DPoses>();
    if (!(rp && rp->poses.find(target)) && (!a || a->playing == 0)) return mesh.rest_pose[node];
    Array<mat4> pose;
    pose.resize(mesh.nodes.size());
    animated_pose(w, target, mesh, a, pose.span());
    apply_ragdoll_pose(w, target, pose.span());
    return pose[node];
}

// ---- IK ----

// One IK chain's goal for this tick, in the mesh's model space.
struct IKGoal {
    u32 chain = 0;   // index into the graph's chains
    f32 weight = 0.0f;
    vec3 target;
    vec3 normal;     // Foot: the ground's normal (zero: keep the foot's animated rotation)
};
struct IKGoals {
    u32 count = 0;
    f32 pelvis_drop = 0.0f;  // model units the foot chains' pelvis goes down
    IKGoal goals[asset::kAnimGraphMaxIK];
    void add(const IKGoal& g) {
        for (u32 i = 0; i < count; ++i)
            if (goals[i].chain == g.chain) {  // two IKTarget3Ds on one chain: the later wins
                goals[i] = g;
                return;
            }
        if (count < asset::kAnimGraphMaxIK) goals[count++] = g;
    }
};

// Gathered by "kit3d_ik" every tick, read by every pose of the tick (the extract, AttachToBone).
struct AnimIK3D {
    HashMap<Entity, IKGoals> goals;
};

vec3 position_of(const mat4& m) { return m.col(3).xyz(); }

// The rotation taking unit `from` onto unit `to` by the shortest arc.
quat rotation_between(vec3 from, vec3 to) {
    const f32 d = math::dot(from, to);
    if (d < -0.9999f) {  // opposite: half a turn about any perpendicular
        vec3 axis = math::cross(from, vec3{1.0f, 0.0f, 0.0f});
        if (math::dot(axis, axis) < 1e-6f) axis = math::cross(from, vec3{0.0f, 1.0f, 0.0f});
        return math::quat_from_axis_angle(math::normalize(axis), math::kPi);
    }
    const vec3 c = math::cross(from, to);
    return math::normalize(quat{c.x, c.y, c.z, 1.0f + d});
}

quat parent_rotation(const render::GpuMesh& mesh, Span<const mat4> pose, u32 node) {
    const i32 p = mesh.nodes[node].parent;
    return p < 0 ? quat::identity() : rotation_of(pose[static_cast<usize>(p)]);
}

// Bends `bone`'s parent (the knee or elbow) and grandparent (the hip or shoulder) so `bone`
// reaches `goal`: the knee angle from the law of cosines, a swing of the hip onto the goal, and a
// twist about the hip-goal line toward `pole` (when not zero). `bone` keeps its model rotation,
// tilted onto `normal` when one is given.
void solve_two_bone(const render::GpuMesh& mesh, Span<GpuMeshNode> local, Span<mat4> pose, u32 bone, vec3 goal, vec3 pole, vec3 normal,
                    f32 weight) {
    const i32 mid = mesh.nodes[bone].parent;
    if (mid < 0 || mesh.nodes[static_cast<usize>(mid)].parent < 0) return;
    const u32 m = static_cast<u32>(mid), r = static_cast<u32>(mesh.nodes[m].parent);
    const vec3 a = position_of(pose[r]), b = position_of(pose[m]), c = position_of(pose[bone]);
    const quat r_rot = rotation_of(pose[r]), m_rot = rotation_of(pose[m]), e_rot = rotation_of(pose[bone]);
    const vec3 t = c + (goal - c) * weight;
    const f32 lab = math::length(b - a), lcb = math::length(c - b);
    if (lab < 1e-5f || lcb < 1e-5f) return;
    const f32 lat = math::clamp(math::length(t - a), math::max(std::fabs(lab - lcb) * 1.0001f, 1e-4f), (lab + lcb) * 0.9999f);

    // 1. The knee: turn the lower bone about the bend axis until the chain spans `lat`.
    const vec3 u = math::normalize(a - b), v = math::normalize(c - b);
    vec3 axis = math::cross(u, v);
    if (math::dot(axis, axis) < 1e-8f) {  // straight: bend toward the pole, or any side
        const vec3 hint = math::dot(pole, pole) > 0.0f ? pole : vec3{0.0f, 0.0f, 1.0f};
        axis = math::cross(u, hint);
        if (math::dot(axis, axis) < 1e-8f) axis = math::cross(u, vec3{1.0f, 0.0f, 0.0f});
    }
    axis = math::normalize(axis);
    const f32 now = std::acos(math::clamp(math::dot(u, v), -1.0f, 1.0f));
    const f32 want = std::acos(math::clamp((lab * lab + lcb * lcb - lat * lat) / (2.0f * lab * lcb), -1.0f, 1.0f));
    const quat knee = math::quat_from_axis_angle(axis, want - now);
    quat m_new = math::normalize(knee * m_rot);
    const vec3 c1 = b + math::rotate(knee, c - b);

    // 2. The swing: the hip turns the chain's end onto the goal.
    const vec3 toward = t - a;
    const quat swing = math::dot(toward, toward) > 1e-10f && math::dot(c1 - a, c1 - a) > 1e-10f
                           ? rotation_between(math::normalize(c1 - a), math::normalize(toward))
                           : quat::identity();
    quat r_new = math::normalize(swing * r_rot);
    m_new = math::normalize(swing * m_new);

    // 3. The twist: the knee turns toward the pole about the hip-goal line.
    if (math::dot(pole, pole) > 0.0f && math::dot(toward, toward) > 1e-10f) {
        const vec3 k = math::normalize(toward);
        const vec3 b2 = math::rotate(swing, b - a);
        const vec3 bp = b2 - k * math::dot(b2, k), pp = pole - k * math::dot(pole, k);
        if (math::dot(bp, bp) > 1e-10f && math::dot(pp, pp) > 1e-10f) {
            const f32 angle = std::atan2(math::dot(math::cross(bp, pp), k), math::dot(bp, pp));
            const quat twist = math::quat_from_axis_angle(k, angle * weight);
            r_new = math::normalize(twist * r_new);
            m_new = math::normalize(twist * m_new);
        }
    }
    quat e_new = e_rot;
    if (math::dot(normal, normal) > 0.0f)
        e_new = math::normalize(math::slerp(quat::identity(), rotation_between({0.0f, 1.0f, 0.0f}, math::normalize(normal)), weight) * e_rot);
    local[r].rotation = math::normalize(math::conjugate(parent_rotation(mesh, pose, r)) * r_new);
    local[m].rotation = math::normalize(math::conjugate(r_new) * m_new);
    local[bone].rotation = math::normalize(math::conjugate(m_new) * e_new);
}

// Turns `bone` so its bone-space `axis` points at `goal`, at most `limit` radians away from the
// animated pose.
void solve_look_at(const render::GpuMesh& mesh, Span<GpuMeshNode> local, Span<mat4> pose, u32 bone, vec3 goal, vec3 axis, f32 limit,
                   f32 weight) {
    const vec3 p = position_of(pose[bone]);
    const quat rot = rotation_of(pose[bone]);
    const vec3 to = goal - p;
    if (math::dot(to, to) < 1e-10f || math::dot(axis, axis) <= 0.0f) return;
    quat q = rotation_between(math::normalize(math::rotate(rot, math::normalize(axis))), math::normalize(to));
    const f32 angle = 2.0f * std::acos(math::clamp(std::fabs(q.w), 0.0f, 1.0f));
    if (angle > limit && angle > 0.0f) q = math::slerp(quat::identity(), q, limit / angle);
    q = math::slerp(quat::identity(), q, weight);
    local[bone].rotation = math::normalize(math::conjugate(parent_rotation(mesh, pose, bone)) * math::normalize(q * rot));
}

// Bends the pose by e's goals: the foot chains' pelvis first, then each chain in graph order.
void apply_ik(World& w, Entity e, const render::GpuMesh& mesh, const asset::AnimGraphView& g, Span<GpuMeshNode> local, Span<mat4> pose) {
    const AnimIK3D* ik = w.try_resource<AnimIK3D>();
    const IKGoals* goals = ik ? ik->goals.find(e) : nullptr;
    if (!goals || goals->count == 0) return;
    if (goals->pelvis_drop > 0.0f) {
        for (const asset::AnimGraphIK& k : g.ik) {
            if (k.kind != asset::AnimIKKind::Foot) continue;
            const u32 pelvis = mesh.find_node(asset::AnimGraphView::text(k.pelvis, sizeof k.pelvis));
            if (pelvis >= mesh.nodes.size()) continue;
            const i32 parent = mesh.nodes[pelvis].parent;
            const vec4 down{0.0f, -goals->pelvis_drop, 0.0f, 0.0f};
            local[pelvis].translation += (parent < 0 ? down : math::inverse(pose[static_cast<usize>(parent)]) * down).xyz();
            render::compose_local(local, pose);
            break;
        }
    }
    for (u32 c = 0; c < g.ik.size(); ++c) {
        const IKGoal* goal = nullptr;
        for (u32 i = 0; i < goals->count; ++i)
            if (goals->goals[i].chain == c) goal = &goals->goals[i];
        if (!goal || !(goal->weight > 0.0f)) continue;
        const asset::AnimGraphIK& k = g.ik[c];
        const u32 bone = mesh.find_node(asset::AnimGraphView::text(k.bone, sizeof k.bone));
        if (bone >= mesh.nodes.size()) continue;
        const vec3 axis{k.axis[0], k.axis[1], k.axis[2]};
        const f32 weight = math::min(goal->weight, 1.0f);
        if (k.kind == asset::AnimIKKind::LookAt)
            solve_look_at(mesh, local, pose, bone, goal->target, axis, k.limit_deg * (math::kPi / 180.0f), weight);
        else
            solve_two_bone(mesh, local, pose, bone, goal->target, axis, goal->normal, weight);
        render::compose_local(local, pose);
    }
}

// "kit3d_ik": every chain's goal for this tick. IKTarget3D entities give the two-bone and look-at
// goals; a foot chain casts down through the physics world below its animated foot, and its
// pelvis goes down by the lowest foot's drop.
void kit3d_ik(Frame& f) {
    World& w = f.world();
    asset::AssetSystem* assets = kit3d_assets(w);
    Anim3DAssets& cache = w.resource<Anim3DAssets>();
    AnimIK3D& ik = w.resource<AnimIK3D>();
    ik.goals.clear();
    f.query<IKTarget3D&, LocalTransform&>().each([&](Entity te, IKTarget3D& it, LocalTransform& tl) {
        it.found = 0;
        const Animator* a = w.alive(it.animator) ? w.try_get<Animator>(it.animator) : nullptr;
        const LocalTransform* al = a ? w.try_get<LocalTransform>(it.animator) : nullptr;
        asset::AnimGraphView g;
        if (!al || a->graph.guid == asset::Guid{} || !cache.graph(assets, a->graph.guid, g)) return;
        u32 chain = ~0u;
        for (u32 i = 0; i < g.ik.size(); ++i)
            if (g.ik[i].kind != asset::AnimIKKind::Foot && asset::AnimGraphView::text(g.ik[i].name, sizeof g.ik[i].name) == it.chain.view())
                chain = i;
        if (chain == ~0u) return;
        vec3 tp, ts, ap, as;
        quat tr, ar;
        world_pose(w, te, tl, tp, tr, ts);
        world_pose(w, it.animator, *al, ap, ar, as);
        const vec3 d = math::rotate(math::conjugate(ar), tp - ap);
        IKGoal goal;
        goal.chain = chain;
        goal.weight = math::clamp(it.weight, 0.0f, 1.0f) * weight_of(g.ik[chain].weight, g.ik[chain].weight_param, a->params);
        goal.target = {as.x != 0.0f ? d.x / as.x : 0.0f, as.y != 0.0f ? d.y / as.y : 0.0f, as.z != 0.0f ? d.z / as.z : 0.0f};
        ik.goals[it.animator].add(goal);
        it.found = 1;
    });

    Physics3D* phys = w.try_resource<Physics3D>();
    if (!phys) return;
    f.query<Animator&, MeshRef&, LocalTransform&>().each([&](Entity e, Animator& a, MeshRef& ref, LocalTransform& t) {
        asset::AnimGraphView g;
        if (a.graph.guid == asset::Guid{} || !cache.graph(assets, a.graph.guid, g)) return;
        bool feet = false;
        for (const asset::AnimGraphIK& k : g.ik) feet = feet || k.kind == asset::AnimIKKind::Foot;
        const render::GpuMesh* mesh = feet ? cache.mesh(assets, ref.mesh.guid) : nullptr;
        if (!mesh) return;
        Array<mat4> pose;
        pose.resize(mesh->nodes.size());
        if (!extended_pose(w, e, *mesh, a, pose.span(), false)) return;
        vec3 pos, scale;
        quat rot;
        world_pose(w, e, t, pos, rot, scale);
        const mat4 model = math::trs(pos, rot, scale);
        const Entity self[] = {e};
        physics::QueryFilter3D filter;
        filter.exclude = Span<const Entity>(self, 1);
        f32 lowest = 0.0f;
        bool any = false;
        IKGoals* goals = nullptr;
        for (u32 c = 0; c < g.ik.size(); ++c) {
            const asset::AnimGraphIK& k = g.ik[c];
            if (k.kind != asset::AnimIKKind::Foot) continue;
            const f32 weight = weight_of(k.weight, k.weight_param, a.params);
            const u32 bone = mesh->find_node(asset::AnimGraphView::text(k.bone, sizeof k.bone));
            if (!(weight > 0.0f) || bone >= mesh->nodes.size()) continue;
            const vec3 foot = (model * vec4{pose[bone].c[3][0], pose[bone].c[3][1], pose[bone].c[3][2], 1.0f}).xyz();
            const std::optional<physics::RayHit3D> hit =
                phys->world.raycast({foot.x, pos.y + k.reach, foot.z}, {0.0f, -1.0f, 0.0f}, 2.0f * k.reach, filter);
            if (!hit) continue;
            const f32 offset = hit->point.y - pos.y;  // the ground under this foot against the entity's
            const vec3 d = math::rotate(math::conjugate(rot), vec3{0.0f, offset, 0.0f});
            IKGoal goal;
            goal.chain = c;
            goal.weight = weight;
            goal.target = position_of(pose[bone]) +
                          vec3{scale.x != 0.0f ? d.x / scale.x : 0.0f, scale.y != 0.0f ? d.y / scale.y : 0.0f, scale.z != 0.0f ? d.z / scale.z : 0.0f};
            goal.normal = math::normalize(math::rotate(math::conjugate(rot), hit->normal));
            if (!goals) goals = &ik.goals[e];
            goals->add(goal);
            lowest = any ? math::min(lowest, offset * weight) : offset * weight;
            any = true;
        }
        if (goals && lowest < 0.0f && scale.y != 0.0f) goals->pelvis_drop = -lowest / scale.y;
    });
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

bool extended_pose(World& w, Entity e, const render::GpuMesh& mesh, const Animator& a, Span<mat4> out, bool ik) {
    if (a.graph.guid == asset::Guid{}) return false;
    Anim3DAssets* cache = w.try_resource<Anim3DAssets>();
    asset::AnimGraphView g;
    if (!cache || !cache->graph(kit3d_assets(w), a.graph.guid, g) || !g.extension) return false;
    const usize n = mesh.nodes.size();
    Array<GpuMeshNode> local;
    local.resize(n);

    // The state machine: the state's clips (a 2D blend's points), the clip fading out.
    ClipSample samples[asset::kAnimGraphMaxBlend2D + 1];
    usize count = 0;
    if (a.playing != 0) {
        ClipSample state[asset::kAnimGraphMaxBlend2D];
        count = state_samples(a, mesh, g, a.time + a.phase, state);
        for (usize i = 0; i < count; ++i) samples[i] = {state[i].clip, state[i].time, state[i].weight * a.blend};
        const u32 prev = a.previous != 0 && a.blend < 1.0f ? find_clip(mesh, a.previous) : ~0u;
        if (prev != ~0u) samples[count++] = {prev, a.previous_time + a.phase, 1.0f - a.blend};
    }
    render::sample_local(mesh, Span<const ClipSample>(samples, count), local.span());

    // Layers, in order, on their masks.
    if (!g.layers.empty()) {
        Array<GpuMeshNode> layer, first;
        Array<u8> on;
        layer.resize(n);
        on.resize(n);
        for (u32 li = 0; li < g.layers.size(); ++li) {
            const asset::AnimGraphLayer& l = g.layers[li];
            const f32 weight = weight_of(l.weight, l.weight_param, a.params);
            const u32 clip = weight > 0.0f ? find_clip(mesh, clip_id(l.clip, sizeof l.clip)) : ~0u;
            if (clip == ~0u) continue;
            for (usize i = 0; i < n; ++i) on[i] = l.mask_count == 0 ? 1 : 0;
            for (u32 m = 0; m < l.mask_count; ++m) {
                const asset::AnimGraphMaskBone& b = g.mask_bones[l.first_mask + m];
                const u32 node = mesh.find_node(asset::AnimGraphView::text(b.bone, sizeof b.bone));
                if (node < n) on[node] = 1;
            }
            for (usize i = 0; i < n; ++i)
                if (mesh.nodes[i].parent >= 0 && on[static_cast<usize>(mesh.nodes[i].parent)]) on[i] = 1;
            const render::GpuMeshClip& gc = mesh.clips[clip];
            for (u32 ci = 0; ci < gc.channel_count; ++ci) {  // only the bones the clip animates
                const u32 node = mesh.channels[gc.first_channel + ci].node;
                if (node < n && on[node] == 1) on[node] = 2;
            }
            const ClipSample at{clip, vec_of(a.layer_time, li), 1.0f};
            render::sample_local(mesh, Span<const ClipSample>(&at, 1), layer.span());
            if (l.additive) {
                first.resize(n);
                const ClipSample start{clip, 0.0f, 1.0f};
                render::sample_local(mesh, Span<const ClipSample>(&start, 1), first.span());
            }
            for (usize i = 0; i < n; ++i) {
                if (on[i] != 2) continue;
                GpuMeshNode& out_n = local[i];
                const GpuMeshNode& lp = layer[i];
                if (l.additive) {
                    const GpuMeshNode& ref = first[i];
                    const quat delta = math::normalize(math::conjugate(ref.rotation) * lp.rotation);
                    out_n.rotation = math::normalize(out_n.rotation * math::slerp(quat::identity(), delta, weight));
                    out_n.translation += (lp.translation - ref.translation) * weight;
                    const vec3 ratio{ref.scale.x != 0.0f ? lp.scale.x / ref.scale.x : 1.0f, ref.scale.y != 0.0f ? lp.scale.y / ref.scale.y : 1.0f,
                                     ref.scale.z != 0.0f ? lp.scale.z / ref.scale.z : 1.0f};
                    out_n.scale = out_n.scale * (vec3{1.0f, 1.0f, 1.0f} + (ratio - vec3{1.0f, 1.0f, 1.0f}) * weight);
                } else {
                    out_n.translation += (lp.translation - out_n.translation) * weight;
                    out_n.scale += (lp.scale - out_n.scale) * weight;
                    out_n.rotation = math::normalize(math::slerp(out_n.rotation, lp.rotation, weight));
                }
            }
        }
    }

    // Root motion: the root keeps each clip's first ground position and heading (the entity moves
    // instead), unless the Animator leaves the motion in the pose.
    RootBone r;
    if (a.root_motion != 0 && count > 0 && root_bone(mesh, g, r)) {
        f32 total = 0.0f;
        for (usize i = 0; i < count; ++i) total += math::max(samples[i].weight, 0.0f);
        vec3 drift;
        f32 turn = 0.0f;
        for (usize i = 0; i < count && total > 0.0f; ++i) {
            if (!(samples[i].weight > 0.0f)) continue;
            vec3 p0, p1;
            quat q0, q1;
            root_at(mesh, r, samples[i].clip, 0.0f, p0, q0);
            root_at(mesh, r, samples[i].clip, samples[i].time, p1, q1);
            const f32 wi = samples[i].weight / total;
            drift += vec3{p1.x - p0.x, 0.0f, p1.z - p0.z} * wi;
            turn += yaw_between(q0, q1) * wi;
        }
        GpuMeshNode& root = local[r.node];
        root.translation -= (math::inverse(r.parent) * vec4{drift.x, drift.y, drift.z, 0.0f}).xyz();
        if (turn != 0.0f)
            root.rotation = math::normalize(math::conjugate(r.parent_rot) * math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, -turn) *
                                            r.parent_rot * root.rotation);
    }

    render::compose_local(local.span(), out);
    if (ik) apply_ik(w, e, mesh, g, local.span(), out);
    return true;
}

void animated_pose(World& w, Entity e, const render::GpuMesh& mesh, const Animator* a, Span<mat4> out, bool ik) {
    if (a && extended_pose(w, e, mesh, *a, out, ik)) return;
    render::ClipSample samples[3];
    const usize n = a && a->playing != 0 ? anim_samples(*a, mesh, [&](u64 id) { return find_clip(mesh, id); }, samples) : 0;
    if (n > 0) {
        pose_samples(mesh, samples, n, out);
        return;
    }
    for (usize i = 0; i < out.size() && i < mesh.rest_pose.size(); ++i) out[i] = mesh.rest_pose[i];
}

void install_anim3d(World& world) {
    world.register_components<AttachToBone, IKTarget3D>();
    world.register_event<AnimEvent3D>(Phase::FixedUpdate_Post);
    if (!world.try_resource<AnimIK3D>()) world.set_resource(AnimIK3D{});
    Anim3DAssets a;
    a.store = mem::make_unique<render::MeshStore>(mem::heap());
    a.store->init(nullptr);
    world.set_resource(static_cast<Anim3DAssets&&>(a));
}

void add_anim3d_systems(World& world) {
    world.add_system(Phase::FixedUpdate_Post, "kit3d_animators", &kit3d_animators);
    world.add_system(Phase::FixedUpdate_Post, "kit3d_ik", &kit3d_ik, "kit3d_animators");
    world.add_system(Phase::FixedUpdate_Post, "kit3d_attach", &kit3d_attach, "kit3d_ik");
    world.run_during_cutscenes(Phase::FixedUpdate_Post, "kit3d_animators");
    world.run_during_cutscenes(Phase::FixedUpdate_Post, "kit3d_ik");
    world.run_during_cutscenes(Phase::FixedUpdate_Post, "kit3d_attach");
}

}  // namespace mce::kit
