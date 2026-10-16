// Skeleton2DAnimator (M-ANIM2D 2): plays a .mcskel2d's animations on its entity's Skeleton2D,
// cross-fades them, runs an AnimGraph over them through the step the 3D Animator uses
// (anim_graph_step.h), raises their events as AnimEvent3D and writes the animated channels into
// Skeleton2D::pose before "kit_skeleton2d" poses the world.

#include <cmath>

#include "anim_graph_step.h"
#include "mce/kit/anim3d.h"
#include "mce/kit/skeleton2d.h"
#include "mce/log.h"

namespace mce::kit {

// World::state_hash hashes component bytes: no padding.
static_assert(sizeof(Skeleton2DAnimator) == 208);

// ------------------------------------------------------------------ graphs

Result<void> Skeleton2DRegistry::define_graph(asset::Guid guid, Span<const byte> payload) {
    if (Result<asset::AnimGraphView> v = asset::AnimGraphView::read(payload); !v) return v.error();
    Graph* g = nullptr;
    for (Graph& x : graphs_)
        if (x.guid == guid) g = &x;
    if (!g) {
        graphs_.push_back(Graph{});
        g = &graphs_.back();
        g->guid = guid;
    }
    g->bytes.clear();
    for (byte b : payload) g->bytes.push_back(b);
    g->defined = true;
    g->failed = false;
    return {};
}

bool Skeleton2DRegistry::graph(asset::Guid guid, asset::AnimGraphView& out) {
    if (guid == asset::Guid{}) return false;
    Graph* g = nullptr;
    for (Graph& x : graphs_)
        if (x.guid == guid) g = &x;
    if (g && g->defined) {
        Result<asset::AnimGraphView> v = asset::AnimGraphView::read(g->bytes.span());
        if (!v) return false;
        out = v.value();
        return true;
    }
    if (!assets_) return false;
    if (!g) {
        graphs_.push_back(Graph{});
        g = &graphs_.back();
        g->guid = guid;
    }
    // Keep one reference; a hot reload kills the handle and the next lookup loads the new generation.
    if (g->handle.generation == 0 || !assets_->is_alive(g->handle)) {
        if (g->failed) return false;
        if (g->handle.generation != 0) assets_->release(g->handle);
        g->handle = {};
        Result<asset::AssetHandle<asset::AnimGraph>> h = assets_->load_sync<asset::AnimGraph>(guid);
        if (!h) {
            MCE_LOG_WARN("kit: animation graph {} cannot be loaded; its Skeleton2DAnimator plays `clip`",
                         asset::to_string(guid).view());
            g->failed = true;
            return false;
        }
        g->handle = asset::AnyHandle(h.value());
    }
    Result<asset::AnimGraphView> v = asset::AnimGraphView::read(
        assets_->resolve(asset::AssetHandle<asset::AnimGraph>{g->handle.index, g->handle.generation}));
    if (!v) return false;
    out = v.value();
    return true;
}

// ------------------------------------------------------------------ blending

Skeleton2DBlend::Skeleton2DBlend() {
    for (Bone2DPose& p : acc) p = Bone2DPose{0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    for (math::Color& c : color) c = math::Color{0.0f, 0.0f, 0.0f, 0.0f};
}

void Skeleton2DBlend::add(const Skeleton2DRegistry::Def& def, u32 anim, f32 t, f32 w) {
    if (anim >= def.animations.size()) return;
    const asset::Skeleton2DAnimation& a = def.animations[anim];
    if (a.duration > 0.0f && t >= a.duration) {
        t = (a.flags & asset::Skeleton2DAnimationFlags::Loop) ? std::fmod(t, a.duration) : a.duration;
    }
    const u32 bones = static_cast<u32>(def.bones.size() < kSkeleton2DBones ? def.bones.size() : kSkeleton2DBones);
    for (u32 i = 0; i < a.timeline_count; ++i) {
        const asset::Skeleton2DTimeline& tl = def.timelines[a.first_timeline + i];
        if (tl.channel == asset::Skeleton2DChannel::IKMix) {
            // An IK constraint's mix (M-ANIM2D 4): summed by weight like a color.
            if (tl.bone >= def.ik.size() || tl.bone >= kSkeleton2DIKCount) continue;
            f32 v[2];
            asset::skeleton2d_sample(def.keys.span().subspan(tl.first_key, tl.key_count), t, v);
            ik_mix[tl.bone] += v[0] * w;
            ik_weight[tl.bone] += w;
            ik_keyed |= 1u << tl.bone;
            continue;
        }
        if (tl.channel > asset::Skeleton2DChannel::Scale || tl.bone >= bones) continue;
        f32 v[2];
        asset::skeleton2d_sample(def.keys.span().subspan(tl.first_key, tl.key_count), t, v);
        Bone2DPose& p = acc[tl.bone];
        math::vec3& wt = weight[tl.bone];
        switch (tl.channel) {
            case asset::Skeleton2DChannel::Rotate:
                p.rotation += v[0] * w;
                wt.x += w;
                keyed[tl.bone] |= 1u;
                break;
            case asset::Skeleton2DChannel::Translate:
                p.x += v[0] * w;
                p.y += v[1] * w;
                wt.y += w;
                keyed[tl.bone] |= 2u;
                break;
            case asset::Skeleton2DChannel::Scale:
                p.scale_x += v[0] * w;
                p.scale_y += v[1] * w;
                wt.z += w;
                keyed[tl.bone] |= 4u;
                break;
            default:
                break;
        }
    }
    // Slot timelines: colors sum by weight, the first keying sample with weight picks the
    // attachment, deform offsets sum by weight into `deform`.
    const u32 slots = static_cast<u32>(def.slots.size() < kSkeleton2DSlotCount ? def.slots.size() : kSkeleton2DSlotCount);
    for (u32 i = 0; i < a.timeline_count; ++i) {
        const asset::Skeleton2DTimeline& tl = def.timelines[a.first_timeline + i];
        if (tl.channel <= asset::Skeleton2DChannel::Scale || tl.channel == asset::Skeleton2DChannel::IKMix || tl.bone >= slots)
            continue;
        if (u64(tl.first_key) + tl.key_count > def.slot_keys.size()) continue;
        const Span<const asset::Skeleton2DSlotKey> keys = def.slot_keys.span().subspan(tl.first_key, tl.key_count);
        u32 ka = 0, kb = 0;
        f32 f = 0.0f;
        asset::skeleton2d_slot_interval(keys, t, ka, kb, f);
        if (tl.channel == asset::Skeleton2DChannel::Attachment) {
            if (!(w > 0.0f) || (slot_keyed[tl.bone] & 2u)) continue;
            const u32 v = keys[ka].value;
            attachment[tl.bone] = v == asset::kSkeleton2DNoAttachmentKey ? asset::kSkeleton2DNoAttachment : static_cast<u8>(v);
            slot_keyed[tl.bone] |= 2u;
            continue;
        }
        if (tl.channel == asset::Skeleton2DChannel::Color) {
            const f32* va = def.values.data() + keys[ka].value;
            const f32* vb = def.values.data() + keys[kb].value;
            f32 c[4];
            for (u32 k = 0; k < 4; ++k) c[k] = va[k] + (vb[k] - va[k]) * f;
            math::Color& acc_c = color[tl.bone];
            acc_c.r += c[0] * w;
            acc_c.g += c[1] * w;
            acc_c.b += c[2] * w;
            acc_c.a += c[3] * w;
            color_weight[tl.bone] += w;
            slot_keyed[tl.bone] |= 1u;
            continue;
        }
        // Deform: 2 offsets per vertex of the mesh the timeline names.
        if (!deform || tl.attachment >= def.attachments.size()) continue;
        const u32 n = 2 * def.attachments[tl.attachment].vertex_count;
        if (deform->offset.size() < def.attachments.size()) {
            const usize had = deform->offset.size();
            deform->offset.resize(def.attachments.size());
            for (usize k = had; k < deform->offset.size(); ++k) deform->offset[k] = ~0u;
        }
        if (deform->offset[tl.attachment] == ~0u) {
            deform->offset[tl.attachment] = static_cast<u32>(deform->values.size());
            deform->values.resize(deform->values.size() + n, 0.0f);
        }
        f32* out = deform->values.data() + deform->offset[tl.attachment];
        const f32* va = def.values.data() + keys[ka].value;
        const f32* vb = def.values.data() + keys[kb].value;
        for (u32 k = 0; k < n; ++k) out[k] += (va[k] + (vb[k] - va[k]) * f) * w;
    }
}

void Skeleton2DBlend::write_slots(const Skeleton2DRegistry::Def& def, Skeleton2DSlots& slots) const {
    const u32 n = static_cast<u32>(def.slots.size() < kSkeleton2DSlotCount ? def.slots.size() : kSkeleton2DSlotCount);
    for (u32 i = 0; i < n; ++i) {
        if (slot_keyed[i] & 2u) slots.attachment[i] = attachment[i];
        if (slot_keyed[i] & 1u) {
            // Samples that did not key the color hold the slot's setup tint.
            const f32 rest = 1.0f - color_weight[i];
            const f32* setup = def.slots[i].tint;
            slots.color[i] = {color[i].r + setup[0] * rest, color[i].g + setup[1] * rest, color[i].b + setup[2] * rest,
                              color[i].a + setup[3] * rest};
        }
    }
}

void Skeleton2DBlend::write_ik(const Skeleton2DRegistry::Def& def, Skeleton2DIK& ik) const {
    const u32 n = static_cast<u32>(def.ik.size() < kSkeleton2DIKCount ? def.ik.size() : kSkeleton2DIKCount);
    for (u32 i = 0; i < n; ++i) {
        if (!(ik_keyed & (1u << i))) continue;
        // Samples that did not key the mix hold the asset's.
        ik.mix[i] = ik_mix[i] + def.ik[i].mix * (1.0f - ik_weight[i]);
    }
}

void Skeleton2DBlend::write(u32 bone_count, Bone2DPose* pose) const {
    const u32 n = bone_count < kSkeleton2DBones ? bone_count : kSkeleton2DBones;
    for (u32 b = 0; b < n; ++b) {
        if (keyed[b] & 1u) pose[b].rotation = acc[b].rotation;
        if (keyed[b] & 2u) {
            pose[b].x = acc[b].x;
            pose[b].y = acc[b].y;
        }
        if (keyed[b] & 4u) {
            const f32 rest = 1.0f - weight[b].z;  // samples that did not key the scale hold it at one
            pose[b].scale_x = acc[b].scale_x + rest;
            pose[b].scale_y = acc[b].scale_y + rest;
        }
    }
}

// ------------------------------------------------------------------ system

namespace {

using Def = Skeleton2DRegistry::Def;

// The animation index of a clip id, or ~0u.
u32 anim_index(const Def* def, u64 id) {
    const i32 i = def ? def->find_animation(id) : -1;
    return i < 0 ? ~0u : static_cast<u32>(i);
}

// Raises the events of animation `clip` with a time in [lo, hi) (or [lo, hi] when `closed`).
void fire_clip_events(Frame& f, Entity e, Skeleton2DAnimator& a, const Def* def, u64 clip, f32 lo, f32 hi,
                      bool closed) {
    const u32 ai = anim_index(def, clip);
    if (ai == ~0u) return;
    const asset::Skeleton2DAnimation& an = def->animations[ai];
    for (u32 i = 0; i < an.event_count; ++i) {
        const asset::Skeleton2DEvent& ev = def->events[an.first_event + i];
        if (ev.t >= lo && (ev.t < hi || (closed && ev.t <= hi))) {
            ++a.events_fired;
            a.last_event = def->string(ev.name);
            f.events().push(AnimEvent3D{e, ev.id});
        }
    }
}

// The clip without a graph: time advances by dt * speed, a new name starts at 0 and cross-fades
// from the old (the 3D Animator's rules), and the clip's events fire as the playhead crosses them.
void play_clip(Frame& f, Entity e, Skeleton2DAnimator& a, const Def* def) {
    const f32 dt = f.dt;
    a.blend_clip = 0;
    a.blend_weight = 0.0f;
    const u64 want = a.clip.empty() ? 0 : StringId(a.clip.view()).value;
    if (want != a.playing || a.restart) {
        a.restart = false;
        animgraph::start_clip(a, want, a.fade);
        return;  // the switching tick shows the clips as they were: both at their start
    }
    const u32 ai = anim_index(def, a.playing);
    const f32 duration = ai == ~0u ? 0.0f : def->animations[ai].duration;
    const bool loops = a.loop && ai != ~0u && (def->animations[ai].flags & asset::Skeleton2DAnimationFlags::Loop);
    if (!a.finished) {
        const f32 t0 = a.time;
        f32 t1 = t0 + dt * a.speed;
        if (t1 > t0) {
            if (duration > 0.0f && t1 >= duration && loops) {
                fire_clip_events(f, e, a, def, a.playing, t0, duration, false);
                t1 = std::fmod(t1, duration);
                fire_clip_events(f, e, a, def, a.playing, 0.0f, t1, false);
            } else if (duration > 0.0f && t1 >= duration) {
                fire_clip_events(f, e, a, def, a.playing, t0, duration, true);
                t1 = std::nextafter(duration, 0.0f);
                a.finished = true;
            } else {
                fire_clip_events(f, e, a, def, a.playing, t0, t1, false);
            }
        }
        a.time = t1;
    }
    if (a.blend < 1.0f) {
        a.previous_time += dt * a.speed;
        a.blend = a.fade > 0.0f ? std::fmin(1.0f, a.blend + dt / a.fade) : 1.0f;
    }
    if (a.blend >= 1.0f) a.previous = 0;
}

// Samples what the animator plays into `blend`: the playing clip (with a blend state's second clip
// or a 2D blend state's points, at the same phase), weight `blend`, and the clip fading out,
// weight 1 - `blend`.
}  // namespace

void skeleton2d_animator_sample(const Def& def, const Skeleton2DAnimator& a, const asset::AnimGraphView* g,
                                Skeleton2DBlend& blend) {
    const u32 clip = anim_index(&def, a.playing);
    const f32 from = clip == ~0u ? 0.0f : def.animations[clip].duration;
    const auto scaled = [&](u32 c) {
        const f32 to = def.animations[c].duration;
        return from > 0.0f ? a.time / from * to : a.time;
    };
    const f32 now = a.previous != 0 && a.blend < 1.0f ? a.blend : 1.0f;
    bool done = false;
    if (g) {
        const u32 si = g->find_state(a.state.view());
        if (si < g->states.size() && g->states[si].blend_2d && g->states[si].blend_count > 0) {
            const asset::AnimGraphState& s = g->states[si];
            f32 w[asset::kAnimGraphMaxBlend2D];
            animgraph::blend2d_weights(*g, s, a.params, w);
            for (u32 i = 0; i < s.blend_count; ++i) {
                const asset::AnimGraphBlendPoint2D& p = g->blend2d[s.first_blend + i];
                const u32 c = w[i] > 0.0f ? anim_index(&def, animgraph::clip_id(p.clip, sizeof p.clip)) : ~0u;
                if (c != ~0u) blend.add(def, c, scaled(c), w[i] * now);
            }
            done = true;
        }
    }
    if (!done && clip != ~0u) {
        const u32 second = a.blend_clip != 0 && a.blend_weight > 0.0f ? anim_index(&def, a.blend_clip) : ~0u;
        if (second == ~0u) {
            blend.add(def, clip, a.time, now);
        } else {
            blend.add(def, clip, a.time, now * (1.0f - a.blend_weight));
            blend.add(def, second, scaled(second), now * a.blend_weight);
        }
    }
    if (now < 1.0f) {
        const u32 prev = anim_index(&def, a.previous);
        if (prev != ~0u) blend.add(def, prev, a.previous_time, 1.0f - now);
    }
}

void kit_skeleton2d_animators(Frame& f) {
    Skeleton2DRegistry* reg = f.try_resource<Skeleton2DRegistry>();
    if (!reg) return;
    const f32 dt = f.dt;
    f.query<Skeleton2DAnimator, Skeleton2D>().each([&](Entity e, Skeleton2DAnimator& a, Skeleton2D& sk) {
        const Def* def = reg->resolve(sk.skeleton.guid);
        asset::AnimGraphView g;
        const bool graphed = a.graph.guid != asset::Guid{} && reg->graph(a.graph.guid, g);
        if (graphed) {
            const bool entered = animgraph::run_graph(
                f, e, a, g, [def](u64 id) { return def ? def->duration(id) : 0.0f; },
                [&](u64 clip, f32 lo, f32 hi, bool closed) { fire_clip_events(f, e, a, def, clip, lo, hi, closed); });
            if (!entered && a.blend < 1.0f) {
                a.previous_time += dt * a.speed;
                a.blend = a.fade > 0.0f ? std::fmin(1.0f, a.blend + dt / a.fade) : 1.0f;
            }
            if (a.blend >= 1.0f) a.previous = 0;
        } else {
            play_clip(f, e, a, def);
        }
        if (!def) return;
        Skeleton2DBlend blend;
        skeleton2d_animator_sample(*def, a, graphed ? &g : nullptr, blend);
        blend.write(static_cast<u32>(def->bones.size()), sk.pose);
        // Keyed IK mixes go to Skeleton2DIK; the first tick adds one, set up from the skeleton.
        if (def->ik_timelines) {
            if (Skeleton2DIK* ik = f.world().try_get_mut<Skeleton2DIK>(e)) {
                if (ik->constraint_count != (def->ik.size() < kSkeleton2DIKCount ? def->ik.size() : kSkeleton2DIKCount))
                    skeleton2d_ik_setup(*def, *ik);
                blend.write_ik(*def, *ik);
            } else {
                Skeleton2DIK fresh;
                skeleton2d_ik_setup(*def, fresh);
                blend.write_ik(*def, fresh);
                f.commands().add<Skeleton2DIK>(e, fresh);
            }
        }
        if (!def->slot_timelines) return;
        // Keyed slots go to Skeleton2DSlots; the first tick adds one, set up from the skeleton.
        if (Skeleton2DSlots* slots = f.world().try_get_mut<Skeleton2DSlots>(e)) {
            if (slots->slot_count == 0 || slots->slot_count != def->slots.size()) skeleton2d_slots_setup(*def, *slots);
            blend.write_slots(*def, *slots);
        } else {
            Skeleton2DSlots fresh;
            skeleton2d_slots_setup(*def, fresh);
            blend.write_slots(*def, fresh);
            f.commands().add<Skeleton2DSlots>(e, fresh);
        }
    });
}

}  // namespace mce::kit
