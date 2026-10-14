#include "mce/audio/components.h"

namespace mce::audio {

namespace {

// A fading voice starts this far below its gain and ramps up in dB, one step per tick.
constexpr f32 kFadeFloorDb = -60.f;

ClipHandle resolve(ClipBank* bank, asset::Guid guid, ClipHandle direct, bool stream = false) {
    if (!guid.is_null()) return bank ? bank->acquire(guid, stream) : ClipHandle{};
    return direct;
}

f32 faded_gain(const AudioSource& s) { return s.fade_s > 0.f && s.fade_t < 1.f ? s.gain_db + kFadeFloorDb * (1.f - s.fade_t) : s.gain_db; }

AudioHandle start(AudioDevice& dev, AudioSource& s, AudioSpatial* sp, ClipHandle clip, math::vec3 position) {
    s.fade_t = s.fade_s > 0.f ? 0.f : 1.f;
    PlayParams p;
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

}  // namespace

void install(World& world, AudioDevice& device, ClipBank* clips, const char* after) {
    world.register_components<AudioSource>();
    world.register_components<AudioSpatial, AudioListener>();
    world.register_event<PlaySfx>(Phase::FixedUpdate_Pre);
    world.set_resource(AudioRef{&device, clips});
    world.set_hooks<AudioSource>(nullptr, &on_source_removed);
    world.add_system(Phase::FixedUpdate_Post, "audio_bridge", &audio_bridge, after);
}

void audio_bridge(Frame& f) {
    const AudioRef& ref = f.resource<AudioRef>();
    AudioDevice* dev = ref.device;
    if (!dev) return;

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

    for (const PlaySfx& ev : f.events().read<PlaySfx>()) {
        PlayParams p;
        p.gain_db = ev.gain_db;
        p.pan = ev.pan;
        p.priority = ev.priority;
        p.bus = ev.bus;
        p.attenuation_id = ev.attenuation_id;
        p.position = ev.position;
        const ClipHandle clip = resolve(ref.clips, ev.clip, ev.clip_handle);
        if (!clip.is_null()) dev->play(clip, p);
    }

    f.query<AudioSource&>().each([&](Entity e, AudioSource& s) {
        AudioSpatial* sp = f.world().try_get_mut<AudioSpatial>(e);
        math::vec3 pos;
        if (sp)
            if (const LocalTransform* t = f.world().try_get<LocalTransform>(e)) pos = t->position;
        if (!s.started) {
            if (!s.play_on_start) return;
            s.handle = start(*dev, s, sp, resolve(ref.clips, s.clip.guid, s.clip_handle, s.bus == AudioBus::Music), pos);
            s.started = true;
            return;
        }
        if (s.handle.is_null()) return;
        // A hot-reloaded asset re-registers as a new clip and the old voice is stopped: a looping
        // source carries on with the new sound.
        if (s.loop && !s.clip.guid.is_null() && ref.clips) {
            const ClipHandle now = ref.clips->find(s.clip.guid);
            if (!(now == s.playing_clip)) {
                dev->stop(s.handle);
                s.handle = start(*dev, s, sp, now, pos);
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
