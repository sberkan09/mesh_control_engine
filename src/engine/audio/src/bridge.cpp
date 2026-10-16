#include "mce/audio/components.h"

namespace mce::audio {

// World::state_hash hashes component bytes: no padding.
static_assert(sizeof(AudioSource) == 80);
static_assert(sizeof(AudioBusEffects) == 16);
static_assert(sizeof(AudioReverb) == 20);

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

AudioHandle start(AudioDevice& dev, AudioSource& s, AudioSpatial* sp, const ClipPick& pick, math::vec3 position) {
    const ClipHandle clip = pick.clip;
    s.fade_t = s.fade_s > 0.f ? 0.f : 1.f;
    s.pick_gain = half_db(pick.gain_db);
    PlayParams p;
    p.pitch = pick.pitch;
    p.gain_db = faded_gain(s);
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

// The scene's effect components onto the device (M-AUDIO2 3). The device sends only changes, so
// this runs every tick; an effect whose component went away goes back to its default.
void apply_effects(Frame& f, AudioRef& ref, AudioDevice& dev) {
    u16 buses = 0;
    f.query<const AudioBusEffects&>().each([&](Entity, const AudioBusEffects& fx) {
        const BusId b = static_cast<BusId>(fx.bus);
        if (b >= kBusCount || (buses & (1u << b)) != 0) return;
        buses = static_cast<u16>(buses | (1u << b));
        BusFilterDesc d;
        d.lowpass_hz = fx.lowpass_hz;
        d.highpass_hz = fx.highpass_hz;
        dev.set_bus_filter(b, d);
        if (b == Bus_Master) return;
        for (u32 u = 0; u < kReverbUnits; ++u) (void)dev.set_bus_send(b, u, u == fx.reverb_unit ? fx.reverb_send_db : -96.f);
    });
    for (u32 b = 0; b < kBusCount; ++b) {
        if ((ref.fx_buses & ~buses & (1u << b)) == 0) continue;
        dev.set_bus_filter(static_cast<BusId>(b), BusFilterDesc{});
        if (b != Bus_Master)
            for (u32 u = 0; u < kReverbUnits; ++u) (void)dev.set_bus_send(static_cast<BusId>(b), u, -96.f);
    }
    ref.fx_buses = buses;

    u8 units = 0;
    f.query<const AudioReverb&>().each([&](Entity, const AudioReverb& r) {
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
    f.query<const AudioLimiter&>().each([&](Entity, const AudioLimiter& l) {
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

}  // namespace

void install(World& world, AudioDevice& device, ClipBank* clips, const char* after) {
    world.register_components<AudioSource>();
    world.register_components<AudioSpatial, AudioListener>();
    world.register_components<AudioBusEffects, AudioReverb, AudioLimiter>();
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
    // Effects before this tick's plays, so a sound starts with the bus as the scene has it.
    apply_effects(f, ref, *dev);

    // The listener first, so this tick's plays and moves spatialize against this tick's ear.
    bool have_listener = false;
    f.query<const AudioListener&>().each([&](Entity e, const AudioListener& al) {
        if (have_listener) return;
        have_listener = true;
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
        if (!s.started) {
            if (!s.play_on_start) return;
            const ClipPick pick = resolve(ref.clips, s.clip.guid, s.clip_handle, play_seed(f.tick, e.index, s.clip.guid), s.bus == AudioBus::Music);
            // The previous scene's source of the same music still fades out: start when it is done.
            if (!pick.clip.is_null() && dev->stream_busy(pick.clip)) return;
            s.handle = start(*dev, s, sp, pick, pos);
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
                s.handle = start(*dev, s, sp, again, pos);
                return;
            }
        }
        if (!dev->is_playing(s.handle)) {
            s.handle = {};  // one-shot finished (or stolen)
            return;
        }
        // Live edits (inspector, gameplay) and the fade-in ramp in; nothing is sent when nothing changed.
        if (s.fade_t < 1.f) s.fade_t = s.fade_s > 0.f ? math::min(1.f, s.fade_t + f.dt / s.fade_s) : 1.f;
        if (const f32 gain = faded_gain(s); gain != s.applied_gain_db) {
            dev->set_handle_gain(s.handle, gain);
            s.applied_gain_db = gain;
        }
        if (s.pan != s.applied_pan) {
            dev->set_handle_pan(s.handle, s.pan);
            s.applied_pan = s.pan;
        }
        if (sp && pos != sp->applied_position) {
            dev->set_handle_position(s.handle, pos);
            sp->applied_position = pos;
        }
    });
}

}  // namespace mce::audio
