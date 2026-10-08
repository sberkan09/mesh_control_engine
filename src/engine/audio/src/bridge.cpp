#include "mce/audio/components.h"

namespace mce::audio {

namespace {

void on_source_removed(World& w, Entity e) {
    AudioRef* ref = w.try_resource<AudioRef>();
    const AudioSource* src = w.try_get<AudioSource>(e);
    if (ref && ref->device && src && !src->handle.is_null()) ref->device->stop(src->handle, 0.05f);
}

}  // namespace

void install(World& world, AudioDevice& device, const char* after) {
    world.register_components<AudioSource>();
    world.register_event<PlaySfx>(Phase::FixedUpdate_Pre);
    world.set_resource(AudioRef{&device});
    world.set_hooks<AudioSource>(nullptr, &on_source_removed);
    world.add_system(Phase::FixedUpdate_Post, "audio_bridge", &audio_bridge, after);
}

void audio_bridge(Frame& f) {
    AudioDevice* dev = f.resource<AudioRef>().device;
    if (!dev) return;

    for (const PlaySfx& ev : f.events().read<PlaySfx>()) {
        PlayParams p;
        p.gain_db = ev.gain_db;
        p.pan = ev.pan;
        p.priority = ev.priority;
        p.bus = ev.bus;
        dev->play(ev.clip, p);
    }

    f.query<AudioSource&>().each([&](Entity, AudioSource& s) {
        if (!s.started) {
            if (!s.play_on_start) return;
            PlayParams p;
            p.gain_db = s.gain_db;
            p.pan = s.pan;
            p.priority = s.priority;
            p.bus = s.bus;
            p.loop = s.loop;
            s.handle = dev->play(s.clip, p);
            s.started = true;
            s.applied_gain_db = s.gain_db;
            s.applied_pan = s.pan;
            return;
        }
        if (s.handle.is_null()) return;
        if (!dev->is_playing(s.handle)) {
            s.handle = {};  // one-shot finished (or stolen)
            return;
        }
        // Live edits (inspector, gameplay) ramp in; nothing is sent when nothing changed.
        if (s.gain_db != s.applied_gain_db) {
            dev->set_handle_gain(s.handle, s.gain_db);
            s.applied_gain_db = s.gain_db;
        }
        if (s.pan != s.applied_pan) {
            dev->set_handle_pan(s.handle, s.pan);
            s.applied_pan = s.pan;
        }
    });
}

}  // namespace mce::audio
