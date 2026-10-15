#include "mce/kit/timeline.h"

#include <cmath>
#include <cstring>

#include "mce/audio/components.h"
#include "mce/ecs/transform.h"
#include "mce/kit/cinematic.h"
#include "mce/kit/components.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/sequence.h"
#include "mce/log.h"
#include "mce/refl/property_path.h"
#include "mce/refl/refl.h"

namespace mce::kit {

using asset::TimelineEase;
using asset::TimelineKey;
using asset::TimelineTrackKind;

namespace {

// A key within a thousandth of a tick of the playhead has been reached (float sums of dt).
f32 tolerance(f32 dt) { return dt > 0.0f ? dt * 1e-3f : 1e-6f; }

// The track's value at `t`: held before the first key and after the last.
}  // namespace

f32 timeline_eased(TimelineEase e, f32 u) {
    switch (e) {
        case TimelineEase::Linear: return u;
        case TimelineEase::Step: return 0.0f;
        case TimelineEase::Smooth: return u * u * (3.0f - 2.0f * u);
        case TimelineEase::EaseIn: return u * u;
        case TimelineEase::EaseOut: return 1.0f - (1.0f - u) * (1.0f - u);
    }
    return u;
}

namespace {

void sample(Span<const TimelineKey> keys, bool quat, u32 count, f32 t, f32 out[4]) {
    const TimelineKey* a = &keys[0];
    const TimelineKey* b = nullptr;
    if (t > a->t) {
        // The last key at or before t (binary search over the time-sorted keys).
        usize lo = 0, hi = keys.size();
        while (hi - lo > 1) {
            const usize mid = (lo + hi) / 2;
            if (keys[mid].t <= t) lo = mid;
            else hi = mid;
        }
        a = &keys[lo];
        if (lo + 1 < keys.size()) b = &keys[lo + 1];
    }
    if (!b || !(b->t > a->t)) {
        std::memcpy(out, a->v, sizeof(f32) * 4);
        return;
    }
    const f32 u = timeline_eased(a->ease, (t - a->t) / (b->t - a->t));
    if (quat) {
        math::quat qa{a->v[0], a->v[1], a->v[2], a->v[3]};
        math::quat qb{b->v[0], b->v[1], b->v[2], b->v[3]};
        const math::quat q = math::slerp(math::normalize(qa), math::normalize(qb), u);
        out[0] = q.x;
        out[1] = q.y;
        out[2] = q.z;
        out[3] = q.w;
        return;
    }
    for (u32 i = 0; i < count; ++i) out[i] = a->v[i] + (b->v[i] - a->v[i]) * u;
}

Entity target_of(const TimelinePlayer& p, Entity self, u8 target) {
    if (target == asset::kTimelineSelf) return self;
    return target < asset::kTimelineBindings ? p.bindings[target] : Entity::null;
}

// Writes `bytes` over the field when it differs, marking the component changed.
void write_field(World& w, Entity e, ecs::ComponentId c, u32 offset, const void* bytes, usize size) {
    byte* base = static_cast<byte*>(w.get_raw_mut(e, c));
    if (!base || std::memcmp(base + offset, bytes, size) == 0) return;
    std::memcpy(base + offset, bytes, size);
    w.touch(e, c);
}

void set_switch(World& w, Entity e, const TimelineRegistry::Track& t, bool on) {
    using K = refl::FieldKind;
    switch (t.kind) {
        case K::Bool: {
            const bool v = on;
            write_field(w, e, t.comp, t.offset, &v, sizeof(v));
            break;
        }
        case K::U8: {
            const u8 v = on ? 1 : 0;
            write_field(w, e, t.comp, t.offset, &v, sizeof(v));
            break;
        }
        case K::U32:
        case K::I32: {
            const u32 v = on ? 1u : 0u;
            write_field(w, e, t.comp, t.offset, &v, sizeof(v));
            break;
        }
        default: break;
    }
}

void set_primary(World& w, Entity cam, bool on) {
    if (!w.alive(cam)) return;
    if (const Camera3DComponent* c = w.try_get<Camera3DComponent>(cam); c && (c->primary != 0) != on)
        w.try_get_mut<Camera3DComponent>(cam)->primary = on ? 1u : 0u;
    if (const Camera2DComponent* c = w.try_get<Camera2DComponent>(cam); c && c->primary != on)
        w.try_get_mut<Camera2DComponent>(cam)->primary = on;
}

// Applies the state tracks (property, activation, camera, subtitle) at `time`.
void apply_state(World& w, TimelineRegistry& reg, const TimelineRegistry::Def& def, Entity self, f32 time, f32 eps) {
    // Copy: the writes below may move the component's storage.
    TimelinePlayer p = *w.try_get<TimelinePlayer>(self);
    i32 subtitle = -1;
    for (const TimelineRegistry::Track& ct : def.tracks) {
        const Span<const TimelineKey> keys = def.keys_of(ct);
        if (keys.empty()) continue;
        switch (ct.desc.kind) {
            case TimelineTrackKind::Property:
            case TimelineTrackKind::Activation: {
                auto& t = const_cast<TimelineRegistry::Track&>(ct);
                if (!reg.bind(w, t, def)) break;
                const Entity e = target_of(p, self, t.desc.target);
                if (!w.alive(e)) break;
                if (t.desc.kind == TimelineTrackKind::Activation) {
                    bool on = false;
                    for (const TimelineKey& k : keys) on |= k.t <= time + eps && time + eps < k.t + k.duration;
                    set_switch(w, e, t, on);
                } else {
                    f32 v[4] = {};
                    sample(keys, (t.desc.flags & asset::TimelineTrackFlags::Quat) != 0, t.desc.value_count, time, v);
                    write_field(w, e, t.comp, t.offset, v, sizeof(f32) * t.desc.value_count);
                }
                break;
            }
            case TimelineTrackKind::Camera: {
                if (keys[0].t > time + eps) break;
                usize cut = 0;
                for (usize i = 0; i < keys.size() && keys[i].t <= time + eps; ++i) cut = i;
                const Entity live = target_of(p, self, keys[cut].target);
                for (const TimelineKey& k : keys) {
                    const Entity cam = target_of(p, self, k.target);
                    if (!(cam == live)) set_primary(w, cam, false);
                }
                set_primary(w, live, true);
                // A key with a blend (v[0] seconds) eases the view over from the previous key's camera.
                const f32 blend = keys[cut].v[0];
                const Entity prev = cut > 0 ? target_of(p, self, keys[cut - 1].target) : Entity{};
                if (w.alive(live) && blend > 0.0f && !prev.is_null() && !(prev == live)) {
                    CameraBlend3D b;
                    if (const CameraBlend3D* had = w.try_get<CameraBlend3D>(live)) b = *had;
                    b.from = prev;
                    b.duration = blend;
                    b.elapsed = time - keys[cut].t;
                    // In a phase a new component goes through the command buffer (in place next tick).
                    if (w.try_get<CameraBlend3D>(live) || !w.in_phase()) w.set(live, b);
                    else w.commands().set(live, b);
                } else if (CameraBlend3D* had = w.alive(live) ? w.try_get_mut<CameraBlend3D>(live) : nullptr) {
                    had->elapsed = had->duration;  // a hard cut: no blend left
                    had->weight = 1.0f;
                }
                break;
            }
            case TimelineTrackKind::Subtitle:
                for (const TimelineKey& k : keys)
                    if (k.t <= time + eps && time + eps < k.t + k.duration) subtitle = static_cast<i32>(&k - def.keys.data());
                break;
            default: break;
        }
    }
    if (TimelinePlayer* live = w.try_get_mut<TimelinePlayer>(self)) live->subtitle = subtitle;
}

// Fires the cue keys (animation, audio, event) with lo < t <= hi; lo < 0 includes the keys at 0.
void fire(Frame& f, const TimelineRegistry::Def& def, Entity self, f32 lo, f32 hi) {
    World& w = f.world();
    const TimelinePlayer p = *w.try_get<TimelinePlayer>(self);
    const bool audio = f.try_resource<audio::AudioRef>() && w.event_queue<audio::PlaySfx>();
    const bool seq = w.event_queue<SeqSignal>() != nullptr;
    for (const TimelineRegistry::Track& t : def.tracks) {
        const TimelineTrackKind kind = t.desc.kind;
        if (kind != TimelineTrackKind::Animation && kind != TimelineTrackKind::Audio && kind != TimelineTrackKind::Event) continue;
        for (const TimelineKey& k : def.keys_of(t)) {
            if (!(k.t > lo && k.t <= hi)) continue;
            switch (kind) {
                case TimelineTrackKind::Animation: {
                    const Entity e = target_of(p, self, t.desc.target);
                    Animator* a = w.alive(e) ? w.try_get_mut<Animator>(e) : nullptr;
                    if (!a) break;
                    a->clip.assign(def.string(k.text));
                    a->loop = k.v[0] != 0.0f;
                    if (k.v[1] >= 0.0f) a->fade = k.v[1];
                    a->restart = true;
                    break;
                }
                case TimelineTrackKind::Audio:
                    if (audio) {
                        audio::PlaySfx sfx;
                        sfx.clip = k.asset();
                        sfx.gain_db = k.v[0];
                        f.events().push(sfx);
                    }
                    break;
                case TimelineTrackKind::Event:
                    f.events().push(TimelineSignal{StringId(k.id), self});
                    if (seq) f.events().push(SeqSignal{StringId(k.id), self});
                    break;
                default: break;
            }
        }
    }
}

}  // namespace

// ------------------------------------------------------------------ registry

StringView TimelineRegistry::Def::string(u32 offset) const {
    if (offset >= strings.size()) return {};
    usize n = 0;
    while (offset + n < strings.size() && strings[offset + n] != '\0') ++n;
    return StringView(strings.data() + offset, n);
}

Result<void> TimelineRegistry::copy(Def& d, asset::Guid guid, Span<const byte> payload) const {
    Result<asset::TimelineView> view = asset::TimelineView::read(payload);
    if (!view) return view.error();
    const asset::TimelineView& v = view.value();
    d.guid = guid;
    d.name.assign(v.name());
    d.duration = v.header->duration;
    d.tracks.clear();
    d.keys.clear();
    d.strings.clear();
    for (const asset::TimelineTrack& t : v.tracks) {
        Track tr;
        tr.desc = t;
        d.tracks.push_back(tr);
    }
    for (const TimelineKey& k : v.keys) d.keys.push_back(k);
    for (char c : v.strings) d.strings.push_back(c);
    return {};
}

Result<void> TimelineRegistry::define(asset::Guid guid, Span<const byte> payload) {
    Def d;
    if (Result<void> r = copy(d, guid, payload); !r) return r;
    for (Def& existing : defs_)
        if (existing.guid == guid) {
            existing = static_cast<Def&&>(d);
            return {};
        }
    defs_.push_back(static_cast<Def&&>(d));
    return {};
}

const TimelineRegistry::Def* TimelineRegistry::resolve(const TimelinePlayer& p) {
    const asset::Guid guid = p.timeline.guid;
    if (guid == asset::Guid{}) return nullptr;
    Def* cached = nullptr;
    for (Def& d : defs_)
        if (d.guid == guid) cached = &d;
    if (cached && (!assets_ || cached->key == asset::kInvalidAssetKey || assets_->generation(cached->key) == cached->generation))
        return cached;
    if (!assets_) return nullptr;
    const bool fresh = cached == nullptr;
    if (fresh) {
        defs_.push_back(Def{});
        cached = &defs_.back();
    }
    const asset::AssetKey key = assets_->find(guid);
    bool loaded = false;
    if (key != asset::kInvalidAssetKey) {
        if (Result<asset::AssetHandle<asset::Timeline>> h = assets_->load_sync<asset::Timeline>(key)) {
            loaded = static_cast<bool>(copy(*cached, guid, assets_->resolve(h.value())));
            cached->key = key;
            cached->generation = h.value().generation;
            assets_->release(asset::AnyHandle(h.value()));
        }
    }
    if (loaded) return cached;
    if (fresh) defs_.pop_back();
    bool warned = false;
    for (const asset::Guid& g : warned_) warned |= g == guid;
    if (!warned) {
        MCE_LOG_WARN("kit: timeline {} cannot be loaded; its players stay idle", asset::to_string(guid).view());
        warned_.push_back(guid);
    }
    return nullptr;
}

bool TimelineRegistry::bind(World& world, Track& t, const Def& d) {
    if (t.resolved) return t.comp != ecs::kInvalidComponent;
    const StringView path = d.string(t.desc.path);
    const usize dot = path.find('.');
    const refl::TypeRecord* type = dot == StringView::npos ? nullptr : refl::find_type(StringId(path.substr(0, dot)));
    const ecs::ComponentId c = type && type->is_component() ? world.find_component_by_refl(type->id.value) : ecs::kInvalidComponent;
    Result<refl::PropertyRef> ref = c != ecs::kInvalidComponent ? refl::resolve_path(*type, path.substr(dot + 1))
                                                                 : Result<refl::PropertyRef>(Error(Errc::NotFound, "kit.timeline"));
    using K = refl::FieldKind;
    bool ok = ref && ref.value().count == 1;
    if (ok) {
        const K k = ref.value().kind;
        if (t.desc.kind == TimelineTrackKind::Activation) {
            ok = k == K::Bool || k == K::U8 || k == K::U32 || k == K::I32;
        } else {
            const u32 n = k == K::F32 ? 1 : k == K::Vec2 ? 2 : k == K::Vec3 ? 3 : (k == K::Vec4 || k == K::Color || k == K::Quat) ? 4 : 0;
            ok = n == t.desc.value_count && ref.value().size == n * sizeof(f32);
        }
    }
    // Retried when the component is not registered yet (a module installs it later).
    if (c == ecs::kInvalidComponent) return false;
    t.resolved = true;
    if (!ok) {
        MCE_LOG_WARN("kit: timeline '{}' track path '{}' does not match a field; the track is skipped", d.name.view(), path);
        t.comp = ecs::kInvalidComponent;
        return false;
    }
    t.comp = c;
    t.offset = ref.value().offset;
    t.kind = ref.value().kind;
    return true;
}

// ------------------------------------------------------------------ player

void timeline_player(Frame& f) {
    TimelineRegistry* reg = f.try_resource<TimelineRegistry>();
    if (!reg) return;
    World& w = f.world();
    Array<Entity> players;
    f.query<const TimelinePlayer>().each([&](Entity e, const TimelinePlayer& p) {
        if (p.playing || p.restart) players.push_back(e);
    });
    const f32 eps = tolerance(f.dt);
    for (Entity e : players) {
        const TimelinePlayer* cp = w.try_get<TimelinePlayer>(e);
        if (!cp) continue;
        const TimelineRegistry::Def* def = reg->resolve(*cp);
        if (!def) continue;
        TimelinePlayer* p = w.try_get_mut<TimelinePlayer>(e);
        if (p->restart) {
            p->restart = false;
            p->time = 0.0f;
            p->fired_to = -1.0f;
            p->finished = false;
            p->playing = true;
        }
        const f32 speed = p->speed > 0.0f ? p->speed : 0.0f;
        const f32 lo = p->fired_to < 0.0f ? -1.0f : p->fired_to + eps;
        f32 time = p->time + f.dt * speed;
        bool wrapped = false, ended = false;
        if (time + eps >= def->duration) {
            if (p->loop) {
                wrapped = true;
                time = std::fmod(time, def->duration);
                if (time + eps >= def->duration) time = 0.0f;
            } else {
                ended = true;
                time = def->duration;
            }
        }
        p->time = time;
        p->fired_to = time;
        if (ended) {
            p->playing = false;
            p->finished = true;
        }
        if (wrapped) {
            fire(f, *def, e, lo, def->duration + eps);
            fire(f, *def, e, -1.0f, time + eps);
        } else {
            fire(f, *def, e, lo, time + eps);
        }
        if (w.try_get<TimelinePlayer>(e)) apply_state(w, *reg, *def, e, time, eps);
    }
}

bool timeline_scrub(World& world, Entity e, f32 seconds) {
    TimelineRegistry* reg = world.try_resource<TimelineRegistry>();
    const TimelinePlayer* cp = world.try_get<TimelinePlayer>(e);
    if (!reg || !cp) return false;
    const TimelineRegistry::Def* def = reg->resolve(*cp);
    if (!def) return false;
    const f32 t = seconds < 0.0f ? 0.0f : seconds > def->duration ? def->duration : seconds;
    TimelinePlayer* p = world.try_get_mut<TimelinePlayer>(e);
    p->time = t;
    p->fired_to = t > 0.0f ? t : -1.0f;
    p->finished = false;
    apply_state(world, *reg, *def, e, t, tolerance(0.0f));
    place_cameras(world, 0.0f);  // rails and look-ats follow the keyed fields at once
    return true;
}

bool timeline_skip(Frame& f, Entity e) {
    World& w = f.world();
    TimelineRegistry* reg = w.try_resource<TimelineRegistry>();
    const TimelinePlayer* cp = w.try_get<TimelinePlayer>(e);
    if (!reg || !cp) return false;
    const TimelineRegistry::Def* def = reg->resolve(*cp);
    if (!def) return false;
    const f32 eps = tolerance(f.dt);
    const f32 lo = cp->fired_to < 0.0f ? -1.0f : cp->fired_to + eps;
    const TimelinePlayer p = *cp;
    const bool seq = w.event_queue<SeqSignal>() != nullptr;
    for (const TimelineRegistry::Track& t : def->tracks) {
        const Span<const TimelineKey> keys = def->keys_of(t);
        if (t.desc.kind == TimelineTrackKind::Event) {
            for (const TimelineKey& k : keys) {
                if (!(k.t > lo)) continue;
                f.events().push(TimelineSignal{StringId(k.id), e});
                if (seq) f.events().push(SeqSignal{StringId(k.id), e});
            }
        } else if (t.desc.kind == TimelineTrackKind::Animation && !keys.empty() && keys[keys.size() - 1].t > lo) {
            const TimelineKey& k = keys[keys.size() - 1];
            const Entity target = target_of(p, e, t.desc.target);
            if (Animator* a = w.alive(target) ? w.try_get_mut<Animator>(target) : nullptr) {
                a->clip.assign(def->string(k.text));
                a->loop = k.v[0] != 0.0f;
                if (k.v[1] >= 0.0f) a->fade = k.v[1];
                a->restart = true;
            }
        }
    }
    TimelinePlayer* live = w.try_get_mut<TimelinePlayer>(e);
    live->time = def->duration;
    live->fired_to = def->duration;
    live->playing = false;
    live->finished = true;
    live->restart = false;
    apply_state(w, *reg, *def, e, def->duration, eps);
    return true;
}

bool timeline_subtitle(World& world, Entity e, StringView& speaker, StringView& text) {
    TimelineRegistry* reg = world.try_resource<TimelineRegistry>();
    const TimelinePlayer* p = world.try_get<TimelinePlayer>(e);
    if (!reg || !p || p->subtitle < 0) return false;
    const TimelineRegistry::Def* def = reg->resolve(*p);
    if (!def || static_cast<usize>(p->subtitle) >= def->keys.size()) return false;
    const TimelineKey& k = def->keys[static_cast<usize>(p->subtitle)];
    text = def->string(k.text);
    speaker = k.text2 == asset::kTimelineNoString ? StringView() : def->string(k.text2);
    return true;
}

void install_timelines(World& world, asset::AssetSystem* assets) {
    if (TimelineRegistry* r = world.try_resource<TimelineRegistry>()) {
        r->set_assets(assets);
        return;
    }
    world.register_components<TimelinePlayer>();
    world.register_event<TimelineSignal>(Phase::FixedUpdate_Post);
    world.set_resource(TimelineRegistry(assets));
    world.add_system(Phase::FixedUpdate_Post, "timeline_player", &timeline_player);
    world.run_during_cutscenes(Phase::FixedUpdate_Post, "timeline_player");
}

}  // namespace mce::kit
