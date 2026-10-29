// M-MIXEDIT 1: the mixer model over a scene's audio components. A setter creates the "Mixer ..." entity, a second
// setter rewrites the same one, a clear removes the component and the empty entity, and every change is one undo entry.

#include <doctest/doctest.h>

#include "mce/editor/mixer_model.h"

using namespace mce;
using namespace mce::editor;
using mce::audio::AudioBus;

TEST_CASE("MixerModel: a setter creates the mixer entity, the next one rewrites it, a clear removes both") {
    EditorScene scene;
    scene.new_scene("mix");
    MixerModel mix(scene);

    audio::AudioBusEffects fx;
    CHECK_FALSE(mix.bus_effects(AudioBus::Music, fx));  // dry by default
    CHECK(fx.lowpass_hz == 0.f);
    CHECK(fx.bus == AudioBus::Music);

    fx.lowpass_hz = 1200.f;
    CHECK(mix.set_bus_effects(AudioBus::Music, fx));
    REQUIRE(scene.doc().size() == 1);
    CHECK(scene.doc().at(0).name.view() == "Mixer Music");

    audio::AudioBusEffects got;
    CHECK(mix.bus_effects(AudioBus::Music, got));
    CHECK(got.lowpass_hz == 1200.f);

    fx.lowpass_hz = 800.f;
    CHECK(mix.set_bus_effects(AudioBus::Music, fx));
    CHECK(scene.doc().size() == 1);  // the same entity
    CHECK_FALSE(mix.set_bus_effects(AudioBus::Music, fx));  // unchanged: no-op

    CHECK(mix.set_bus_effects(AudioBus::SFX, audio::AudioBusEffects()));
    CHECK(scene.doc().size() == 2);  // another bus, another entity

    CHECK(mix.clear_bus_effects(AudioBus::Music));
    CHECK(scene.doc().size() == 1);
    CHECK_FALSE(mix.clear_bus_effects(AudioBus::Music));
    CHECK_FALSE(mix.bus_effects(AudioBus::Music, got));
}

TEST_CASE("MixerModel: reverb units, the limiter and ducks are independent and undoable") {
    EditorScene scene;
    scene.new_scene("mix");
    MixerModel mix(scene);

    audio::AudioReverb rv;
    rv.room_size = 0.9f;
    CHECK(mix.set_reverb(1, rv));
    audio::AudioReverb back;
    CHECK_FALSE(mix.reverb(0, back));
    CHECK(mix.reverb(1, back));
    CHECK(back.unit == 1);
    CHECK(back.room_size == doctest::Approx(0.9f));

    audio::AudioLimiter lim;
    lim.ceiling_db = -3.f;
    CHECK(mix.set_limiter(lim));
    audio::AudioLimiter lb;
    CHECK(mix.limiter(lb));
    CHECK(lb.ceiling_db == -3.f);

    audio::AudioDuck d;
    d.depth_db = -18.f;
    CHECK(mix.set_duck(AudioBus::Music, d));
    audio::AudioDuck db;
    CHECK(mix.duck(AudioBus::Music, db));
    CHECK(db.depth_db == -18.f);
    CHECK_FALSE(mix.duck(AudioBus::SFX, db));

    CHECK(scene.doc().size() == 3);
    // One undo entry per change: undoing the duck leaves the limiter and the reverb.
    REQUIRE(scene.undo());
    CHECK_FALSE(mix.duck(AudioBus::Music, db));
    CHECK(mix.limiter(lb));
    CHECK(scene.doc().size() == 2);
    REQUIRE(scene.redo());
    CHECK(mix.duck(AudioBus::Music, db));
}

TEST_CASE("MixerModel: clearing a reverb unit, the limiter or a duck removes only that item") {
    EditorScene scene;
    scene.new_scene("mix");
    MixerModel mix(scene);

    audio::AudioReverb rv;
    rv.room_size = 0.5f;
    REQUIRE(mix.set_reverb(0, rv));
    REQUIRE(mix.set_reverb(1, rv));
    REQUIRE(mix.set_limiter(audio::AudioLimiter()));
    REQUIRE(mix.set_duck(AudioBus::Music, audio::AudioDuck()));
    REQUIRE(mix.set_duck(AudioBus::SFX, audio::AudioDuck()));
    CHECK(scene.doc().size() == 5);

    CHECK(mix.clear_reverb(1));
    CHECK_FALSE(mix.clear_reverb(1));  // already gone
    audio::AudioReverb got;
    CHECK(mix.reverb(0, got));
    CHECK_FALSE(mix.reverb(1, got));

    CHECK(mix.clear_duck(AudioBus::Music));
    audio::AudioDuck d;
    CHECK_FALSE(mix.duck(AudioBus::Music, d));
    CHECK(mix.duck(AudioBus::SFX, d));

    audio::AudioLimiter lim;
    CHECK(mix.clear_limiter());
    CHECK_FALSE(mix.clear_limiter());
    CHECK_FALSE(mix.limiter(lim));
    CHECK(scene.doc().size() == 2);  // reverb 0 and the SFX duck stay

    // Each clear was one undo entry.
    REQUIRE(scene.undo());
    CHECK(mix.limiter(lim));
}

TEST_CASE("MixerModel: writes to one item merge into a single undo entry until the undo is sealed") {
    EditorScene scene;
    scene.new_scene("mix");
    MixerModel mix(scene);

    audio::AudioLimiter lim;
    lim.ceiling_db = -2.f;
    CHECK(mix.set_limiter(lim));
    lim.ceiling_db = -4.f;  // a slider drag: several writes
    CHECK(mix.set_limiter(lim));
    lim.ceiling_db = -6.f;
    CHECK(mix.set_limiter(lim));
    scene.seal_undo();
    lim.ceiling_db = -8.f;
    CHECK(mix.set_limiter(lim));

    audio::AudioLimiter got;
    REQUIRE(scene.undo());  // the drag after the seal
    CHECK(mix.limiter(got));
    CHECK(got.ceiling_db == -6.f);
    REQUIRE(scene.undo());  // the earlier drag and the creation, one entry
    CHECK_FALSE(mix.limiter(got));
    CHECK(scene.doc().size() == 0);
}
