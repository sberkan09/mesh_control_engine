#include <cmath>

#include "mce/audio/components.h"

namespace mce::audio {

// World::state_hash hashes component bytes: no padding.
static_assert(sizeof(AudioSource) == 80);
static_assert(sizeof(AudioBusEffects) == 16);
static_assert(sizeof(AudioReverb) == 20);
static_assert(sizeof(AudioDuck) == 20);
static_assert(sizeof(AudioSnapshot) == 20);
static_assert(sizeof(AudioOcclusion) == 16);
static_assert(sizeof(AudioDoppler) == 8);

namespace {

// A fading voice starts this far below its gain and ramps up in dB, one step per tick.
constexpr f32 kFadeFloorDb = -60.f;

// A play's seed: the tick, which source (an entity, or an event's place in the tick) and the asset.
// Sound sets pick from it, so a replay of the same ticks picks the same clips (M-AUDIO2 2).
u64 play_seed(u64 tick, u64 source, asset::Guid guid) {
    return tick * 0x9E3779B97F4A7C15ull ^ (source + 1) * 0xC2B2AE3D27D4EB4Full ^ guid.hi ^ (guid.lo << 1);
}

ClipPick resolve(ClipBank* bank, asset::Guid guid, ClipHandle direct, u64 seed, bool stream = false) {
    if (!guid.is_null()) return bank ? bank->pick(guid, seed, stream) : ClipPick{};
    ClipPick p;
    p.clip = direct;
    return p;
}

f32 base_gain(const AudioSource& s) { return s.gain_db + 0.5f * static_cast<f32>(s.pick_gain); }
f32 faded_gain(const AudioSource& s) { return s.fade_s > 0.f && s.fade_t < 1.f ? base_gain(s) + kFadeFloorDb * (1.f - s.fade_t) : base_gain(s); }

i8 half_db(f32 db) {
    const f32 h = db * 2.f;
    return static_cast<i8>(h < -127.f ? -127.f : h > 127.f ? 127.f : (h < 0.f ? h - 0.5f : h + 0.5f));
}

u16 rate_of(f32 pitch) { return pitch == 1.f ? u16(0) : static_cast<u16>(std::lround(math::clamp(pitch, kMinPitch, kMaxPitch) * 4096.f)); }
f32 pitch_of(const AudioSource& s) { return s.pick_rate == 0 ? 1.f : static_cast<f32>(s.pick_rate) / 4096.f; }

// `extra_db`: occlusion's cut, on top of the source's own gain.
AudioHandle start(AudioDevice& dev, AudioSource& s, AudioSpatial* sp, const ClipPick& pick, math::vec3 position, f32 extra_db) {
    const ClipHandle clip = pick.clip;
    s.fade_t = s.fade_s > 0.f ? 0.f : 1.f;
    s.pick_gain = half_db(pick.gain_db);
    s.pick_rate = rate_of(pick.pitch);
    PlayParams p;
    p.pitch = pick.pitch;
    p.gain_db = faded_gain(s) + extra_db;
    p.pan = s.pan;
    p.priority = s.priority;
    p.bus = static_cast<BusId>(s.bus);
    p.loop = s.loop;
    if (sp) {
        p.attenuation_id = sp->attenuation_id;
        p.position = position;
        sp->applied_position = position;
    }
    s.playing_clip = clip;
    s.applied_gain_db = p.gain_db;
    s.applied_pan = s.pan;
    return clip.is_null() ? AudioHandle{} : dev.play(clip, p);
}

void on_source_removed(World& w, Entity e) {
    AudioRef* ref = w.try_resource<AudioRef>();
    const AudioSource* src = w.try_get<AudioSource>(e);
    if (ref && ref->device && src && !src->handle.is_null()) ref->device->stop(src->handle, src->fade_s > 0.05f ? src->fade_s : 0.05f);
}

}  // namespace

void install(World& world, AudioDevice& device, ClipBank* clips, const char* after) {
    world.register_components<AudioSource>();
    world.register_components<AudioSpatial, AudioListener>();
    world.register_components<AudioOcclusion, AudioDoppler>();
    world.register_components<AudioBusEffects, AudioReverb, AudioLimiter>();
    world.register_components<AudioDuck, AudioSnapshot>();
    world.register_event<PlaySfx>(Phase::FixedUpdate_Pre);
    world.set_resource(AudioRef{&device, clips});
    world.set_hooks<AudioSource>(nullptr, &on_source_removed);
    world.add_system(Phase::FixedUpdate_Post, "audio_bridge", &audio_bridge, after);
    world.run_during_cutscenes(Phase::FixedUpdate_Post, "audio_bridge");
}

void audio_bridge(Frame& f) {
    AudioRef& ref = f.resource<AudioRef>();
    AudioDevice* dev = ref.device;
    if (!dev) return;
    // The mix before this tick's plays, so a sound starts with the bus as the scene has it.
    update_mix(f.world(), static_cast<f32>(f.dt), false);

    // The listener first, so this tick's plays and moves spatialize against this tick's ear.
    bool have_listener = false;
    Entity listener_entity;
    f.query<const AudioListener&>().each([&](Entity e, const AudioListener& al) {
        if (have_listener) return;
        have_listener = true;
        listener_entity = e;
        Listener l;
        l.gain_db = al.gain_db;
        if (const LocalTransform* t = f.world().try_get<LocalTransform>(e)) {
            l.position = t->position;
            l.forward = math::rotate(t->rotation, math::vec3{0.f, 0.f, -1.f});
            l.up = math::rotate(t->rotation, math::vec3{0.f, 1.f, 0.f});
        }
        const Listener& cur = dev->listener();
        if (l.position != cur.position || l.forward != cur.forward || l.up != cur.up || l.gain_db != cur.gain_db)
            dev->set_listener(l);
    });
    // The listener's movement this tick, for Doppler (none on its first tick).
    const f32 dt = static_cast<f32>(f.dt);
    const math::vec3 ear = dev->listener().position;
    const math::vec3 ear_velocity = ref.have_last_listener && dt > 0.f ? (ear - ref.last_listener) * (1.f / dt) : math::vec3{};
    ref.last_listener = ear;
    ref.have_last_listener = have_listener;

    u64 nth = 0;
    for (const PlaySfx& ev : f.events().read<PlaySfx>()) {
        const ClipPick pick = resolve(ref.clips, ev.clip, ev.clip_handle, play_seed(f.tick, nth++ | (1ull << 40), ev.clip));
        PlayParams p;
        p.gain_db = ev.gain_db + pick.gain_db;
        p.pitch = pick.pitch;
        p.pan = ev.pan;
        p.priority = ev.priority;
        p.bus = ev.bus;
        p.attenuation_id = ev.attenuation_id;
        p.position = ev.position;
        if (!pick.clip.is_null()) dev->play(pick.clip, p);
    }

    f.query<AudioSource&>().each([&](Entity e, AudioSource& s) {
        AudioSpatial* sp = f.world().try_get_mut<AudioSpatial>(e);
        math::vec3 pos;
        if (sp)
            if (const LocalTransform* t = f.world().try_get<LocalTransform>(e)) pos = t->position;
        // Occlusion: the ray decides where the amount heads; it gets there over 100 ms.
        f32 occluded_db = 0.f, occluded_hz = 0.f;
        AudioOcclusion* oc = sp ? f.world().try_get_mut<AudioOcclusion>(e) : nullptr;
        if (oc) {
            const bool blocked = have_listener && ref.occlusion && ref.occlusion(f.world(), e, listener_entity, ear, pos, oc->mask);
            const f32 step = dt / 0.1f;
            oc->amount = blocked ? math::min(1.f, oc->amount + step) : math::max(0.f, oc->amount - step);
            occluded_db = oc->amount * oc->gain_db;
            if (oc->amount > 0.f) {
                const f32 open = std::log(20000.f), shut = std::log(math::clamp(oc->lowpass_hz, 20.f, 20000.f));
                occluded_hz = std::exp(open + oc->amount * (shut - open));
            }
        }
        if (!s.started) {
            if (!s.play_on_start) return;
            const ClipPick pick = resolve(ref.clips, s.clip.guid, s.clip_handle, play_seed(f.tick, e.index, s.clip.guid), s.bus == AudioBus::Music);
            // The previous scene's source of the same music still fades out: start when it is done.
            if (!pick.clip.is_null() && dev->stream_busy(pick.clip)) return;
            s.handle = start(*dev, s, sp, pick, pos, occluded_db);
            if (oc) dev->set_handle_lowpass(s.handle, occluded_hz);
            // A new voice plays at the source's own pitch: Doppler sends its shift from the next tick.
            if (AudioDoppler* dp = sp ? f.world().try_get_mut<AudioDoppler>(e) : nullptr) dp->applied = 1.f;
            s.started = true;
            return;
        }
        if (s.handle.is_null()) return;
        // A hot-reloaded asset re-registers as a new clip and the old voice is stopped: a looping
        // source carries on with the new sound.
        // (A sound set keeps the member it picked: the set's own clip is null.)
        if (s.loop && !s.clip.guid.is_null() && ref.clips && !ref.clips->is_set(s.clip.guid)) {
            const ClipHandle now = ref.clips->find(s.clip.guid);
            if (!(now == s.playing_clip)) {
                dev->stop(s.handle);
                ClipPick again;
                again.clip = now;
                again.pitch = pitch_of(s);
                s.handle = start(*dev, s, sp, again, pos, occluded_db);
                if (AudioDoppler* dp = sp ? f.world().try_get_mut<AudioDoppler>(e) : nullptr) dp->applied = 1.f;
                return;
            }
        }
        if (!dev->is_playing(s.handle)) {
            s.handle = {};  // one-shot finished (or stolen)
            return;
        }
        // Live edits (inspector, gameplay) and the fade-in ramp in; nothing is sent when nothing changed.
        if (s.fade_t < 1.f) s.fade_t = s.fade_s > 0.f ? math::min(1.f, s.fade_t + f.dt / s.fade_s) : 1.f;
        if (oc) dev->set_handle_lowpass(s.handle, occluded_hz);
        if (const f32 gain = faded_gain(s) + occluded_db; gain != s.applied_gain_db) {
            dev->set_handle_gain(s.handle, gain);
            s.applied_gain_db = gain;
        }
        if (s.pan != s.applied_pan) {
            dev->set_handle_pan(s.handle, s.pan);
            s.applied_pan = s.pan;
        }
        // Doppler from both movements along the line between them, before the new position lands.
        if (AudioDoppler* dp = sp ? f.world().try_get_mut<AudioDoppler>(e) : nullptr; dp && have_listener && dt > 0.f) {
            constexpr f32 kSpeedOfSound = 343.f;
            const math::vec3 to_source = pos - ear;
            const f32 dist = math::length(to_source);
            const math::vec3 source_velocity = (pos - sp->applied_position) * (1.f / dt);
            f32 shift = 1.f;
            // Faster than sound in one tick is a teleport, a spawn or a scene change, not a motion:
            // that tick keeps the shift it had instead of chirping.
            const bool jumped = math::length(source_velocity) > kSpeedOfSound || math::length(ear_velocity) > kSpeedOfSound;
            if (jumped) {
                shift = dp->applied;
            } else if (dist > 1e-4f && dp->factor > 0.f) {
                const math::vec3 dir = to_source * (1.f / dist);
                const f32 source_closing = -math::dot(source_velocity, dir) * dp->factor;
                const f32 ear_closing = math::dot(ear_velocity, dir) * dp->factor;
                shift = math::clamp((kSpeedOfSound + ear_closing) / math::max(kSpeedOfSound - source_closing, 1.f), 0.5f, 2.f);
            }
            if (std::fabs(shift - dp->applied) > 1e-3f) {
                dev->set_handle_pitch(s.handle, pitch_of(s) * shift);
                dp->applied = shift;
            }
        }
        if (sp && pos != sp->applied_position) {
            dev->set_handle_position(s.handle, pos);
            sp->applied_position = pos;
        }
    });
}

// The scene's mix components onto the device (M-AUDIO2 3–4). The device sends only changes, so
// this runs every tick; an effect whose component went away goes back to its default.
void update_mix(World& world, f32 dt, bool paused) {
    AudioRef* ref_ptr = world.try_resource<AudioRef>();
    if (!ref_ptr || !ref_ptr->device) return;
    AudioRef& ref = *ref_ptr;
    AudioDevice& dev = *ref.device;

    // Filters and sends from AudioBusEffects (the first entity per bus wins).
    BusFilterDesc filter[kBusCount];
    u16 effects = 0;
    world.query<const AudioBusEffects&>().each([&](Entity, const AudioBusEffects& fx) {
        const BusId b = static_cast<BusId>(fx.bus);
        if (b >= kBusCount || (effects & (1u << b)) != 0) return;
        effects = static_cast<u16>(effects | (1u << b));
        filter[b].lowpass_hz = fx.lowpass_hz;
        filter[b].highpass_hz = fx.highpass_hz;
        if (b == Bus_Master) return;
        for (u32 u = 0; u < kReverbUnits; ++u) (void)dev.set_bus_send(b, u, u == fx.reverb_unit ? fx.reverb_send_db : -96.f);
    });
    const u16 sends = static_cast<u16>(effects & ~1u);
    for (u32 b = 1; b < kBusCount; ++b)
        if ((ref.fx_sends & ~sends & (1u << b)) != 0)
            for (u32 u = 0; u < kReverbUnits; ++u) (void)dev.set_bus_send(static_cast<BusId>(b), u, -96.f);
    ref.fx_sends = sends;

    // Snapshots: advance each fade, then blend into the bus's trim and filters. A snapshot's filter
    // moves the cutoff from where the bus has it (open when off) toward its own, in octaves.
    AudioRef::SnapshotFade fades[AudioRef::kMaxSnapshots];
    u32 count = 0;
    f32 trim[kBusCount] = {};
    u16 snapped = 0;
    constexpr f32 kLowOpen = 20000.f, kHighOpen = 20.f;
    f32 lowpass[kBusCount], highpass[kBusCount];
    for (u32 b = 0; b < kBusCount; ++b) {
        lowpass[b] = filter[b].lowpass_hz > 0.f ? filter[b].lowpass_hz : kLowOpen;
        highpass[b] = filter[b].highpass_hz > 0.f ? filter[b].highpass_hz : kHighOpen;
    }
    world.query<const AudioSnapshot&>().each([&](Entity e, const AudioSnapshot& snap) {
        const BusId b = static_cast<BusId>(snap.bus);
        if (b >= kBusCount || count == AudioRef::kMaxSnapshots) return;
        f32 w = 0.f;
        for (u32 i = 0; i < ref.snapshot_count; ++i)
            if (ref.snapshots[i].entity == e) w = ref.snapshots[i].weight;
        const f32 target = snap.active || (snap.while_paused && paused) ? 1.f : 0.f;
        const f32 step = snap.fade_s > 0.f ? dt / snap.fade_s : 1.f;
        w = target > w ? math::min(target, w + step) : math::max(target, w - step);
        fades[count].entity = e;
        fades[count].weight = w;
        ++count;
        snapped = static_cast<u16>(snapped | (1u << b));
        if (w <= 0.f) return;
        trim[b] += w * snap.gain_db;
        if (snap.lowpass_hz > 0.f)
            lowpass[b] = math::min(lowpass[b], std::exp(std::log(lowpass[b]) + w * (std::log(math::max(snap.lowpass_hz, 20.f)) - std::log(lowpass[b]))));
        if (snap.highpass_hz > 0.f)
            highpass[b] = math::max(highpass[b], std::exp(std::log(highpass[b]) + w * (std::log(math::max(snap.highpass_hz, 20.f)) - std::log(highpass[b]))));
    });
    for (u32 i = 0; i < count; ++i) ref.snapshots[i] = fades[i];
    ref.snapshot_count = count;
    for (u32 b = 0; b < kBusCount; ++b)
        if (((snapped | ref.fx_trims) & (1u << b)) != 0) dev.set_bus_trim(static_cast<BusId>(b), trim[b]);
    ref.fx_trims = snapped;

    const u16 filtered = static_cast<u16>(effects | snapped);
    for (u32 b = 0; b < kBusCount; ++b) {
        if (((filtered | ref.fx_buses) & (1u << b)) == 0) continue;
        BusFilterDesc d;
        d.lowpass_hz = lowpass[b] >= kLowOpen ? filter[b].lowpass_hz : lowpass[b];
        d.highpass_hz = highpass[b] <= kHighOpen ? filter[b].highpass_hz : highpass[b];
        dev.set_bus_filter(static_cast<BusId>(b), d);
    }
    ref.fx_buses = filtered;

    // Ducking (the first entity per ducked bus wins).
    u8 ducks = 0;
    world.query<const AudioDuck&>().each([&](Entity, const AudioDuck& duck) {
        const BusId b = static_cast<BusId>(duck.bus);
        if (b == Bus_Master || b >= Bus_User0 || (ducks & (1u << b)) != 0) return;
        DuckDesc d;
        d.enabled = true;
        d.key = static_cast<BusId>(duck.key);
        d.depth_db = duck.depth_db;
        d.threshold_db = duck.threshold_db;
        d.attack_ms = duck.attack_ms;
        d.release_ms = duck.release_ms;
        if (dev.set_bus_duck(b, d)) ducks = static_cast<u8>(ducks | (1u << b));
    });
    for (u32 b = 1; b < Bus_User0; ++b)
        if ((ref.fx_ducks & ~ducks & (1u << b)) != 0) {
            DuckDesc off = dev.bus_duck(static_cast<BusId>(b));
            off.enabled = false;
            (void)dev.set_bus_duck(static_cast<BusId>(b), off);
        }
    ref.fx_ducks = ducks;

    // Reverb units and the limiter.
    u8 units = 0;
    world.query<const AudioReverb&>().each([&](Entity, const AudioReverb& r) {
        if (r.unit >= kReverbUnits || (units & (1u << r.unit)) != 0) return;
        units = static_cast<u8>(units | (1u << r.unit));
        ReverbDesc d;
        d.enabled = true;
        d.room_size = r.room_size;
        d.damping = r.damping;
        d.width = r.width;
        d.wet_db = r.wet_db;
        (void)dev.set_reverb(r.unit, d);
    });
    for (u32 u = 0; u < kReverbUnits; ++u)
        if ((ref.fx_reverbs & ~units & (1u << u)) != 0) (void)dev.set_reverb(u, ReverbDesc{});
    ref.fx_reverbs = units;

    bool limiter = false;
    world.query<const AudioLimiter&>().each([&](Entity, const AudioLimiter& l) {
        if (limiter) return;
        limiter = true;
        LimiterDesc d;
        d.enabled = true;
        d.ceiling_db = l.ceiling_db;
        d.release_ms = l.release_ms;
        dev.set_limiter(d);
    });
    if (ref.fx_limiter && !limiter) dev.set_limiter(LimiterDesc{});
    ref.fx_limiter = limiter;
}

}  // namespace mce::audio
