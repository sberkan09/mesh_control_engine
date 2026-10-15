#pragma once
// Timelines (FR-ENT-110, M-CINEMA 1): keyed tracks played against the fixed tick. A .mctimeline
// asset (asset::TimelineView) holds the tracks; a TimelinePlayer component plays one on its entity
// and up to eight bound entities.
//
//   kit::install_timelines(world, &assets);   // registry + "timeline_player" (install_stock does it)
//   ... an entity with TimelinePlayer{timeline = guid of intro.mctimeline, bindings = {door, cam_a, ...}}
//
// The player is one FixedUpdate_Post system, registered before kit3d's animators so an animation
// key starts its clip on the same tick. Every tick a playing player advances `time` by dt × speed,
// then:
//   - fires the keys the playhead crossed: audio (an audio::PlaySfx, played by audio_bridge on the
//     next tick), animation (sets the target's Animator clip and restarts it) and event (a
//     TimelineSignal, plus a kit::SeqSignal when sequences are installed, so a sequence can
//     wait_event on a timeline beat);
//   - samples the state tracks at `time`: property (writes the reflected field, interpolated by
//     each key's ease), activation (1 inside a range, else 0), camera (the cut's camera is the
//     primary Camera3DComponent / Camera2DComponent, the track's other cameras are not; a cut
//     with a blend writes a kit::CameraBlend3D from the previous cut's camera, cinematic.h) and
//     subtitle (`subtitle` names the line showing; timeline_subtitle reads its text).
// A key at time t fires on the first tick whose time reaches t (within a thousandth of a tick, the
// tolerance sequences use), so timing is tick math: a key at 0.5 s fires on the 30th tick at 60 Hz.
// At the end a non-looping player stops with `finished` set; a looping one wraps and fires the
// keys again. Everything the player knows lives in the component, so a timeline is deterministic,
// in the state hash and replays, and a save resumes it on the same tick.
//
// Scrubbing (timeline_scrub) moves the playhead and applies the state tracks at the new time
// without firing keys; playing on from there fires the keys after it.

#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/ecs/world.h"
#include "mce/refl/macros.h"

namespace mce::kit {

// Plays a .mctimeline (kit::install_timelines). `bindings` are the entities the tracks with a
// numeric target drive ("$<index>" in a scene); a track whose target is "self" drives this entity.
// Set `restart` to play again from the start (the engine clears it). `fired_to` and `subtitle` are
// runtime state; the playback fields carry the Save flag (give the entity a SaveKey).
struct TimelinePlayer {
    asset::AssetRef<asset::Timeline> timeline;
    Entity bindings[asset::kTimelineBindings];
    f32 time = 0.0f;       // seconds into the timeline
    f32 speed = 1.0f;      // playback rate (>= 0)
    f32 fired_to = -1.0f;  // runtime: keys up to this time have fired (< 0: none yet)
    i32 subtitle = -1;     // runtime: the key index of the subtitle line showing, -1 when none
    bool playing = true;
    bool loop = false;
    bool finished = false;  // runtime: a non-looping timeline reached its end
    bool restart = false;   // set to play from the start (the engine clears it)
    u32 pad_ = 0;
};
MCE_COMPONENT(TimelinePlayer, timeline, (bindings, Help{"entities the timeline's numbered targets drive"}),
              (time, Save, Help{"seconds into the timeline (kit::timeline_scrub moves it and applies the tracks)"}),
              (speed, Range{0, 8}), (fired_to, Save, ReadOnly), (subtitle, Save, ReadOnly), (playing, Save),
              (loop, Help{"wrap at the end and play again"}), (finished, Save, ReadOnly),
              (restart, Help{"play again from the start (the engine clears it)"}))

// An event track key: `id` is the key's name, `entity` the playing entity. Readable from the next
// tick, like every event.
struct TimelineSignal {
    StringId id;
    Entity entity;
};

class TimelineRegistry {
public:
    TimelineRegistry() = default;
    explicit TimelineRegistry(asset::AssetSystem* assets) : assets_(assets) {}
    void set_assets(asset::AssetSystem* assets) { assets_ = assets; }
    asset::AssetSystem* assets() const { return assets_; }

    // A track, with its reflected field resolved against this world on first use.
    struct Track {
        asset::TimelineTrack desc;
        ecs::ComponentId comp = ecs::kInvalidComponent;  // Property / Activation
        u32 offset = 0;
        refl::FieldKind kind = refl::FieldKind::F32;
        bool resolved = false;
    };
    struct Def {
        asset::Guid guid;
        asset::AssetKey key = asset::kInvalidAssetKey;
        u32 generation = 0;
        FixedString<32> name;
        f32 duration = 0.0f;
        Array<Track> tracks;
        Array<asset::TimelineKey> keys;
        Array<char> strings;
        Span<const asset::TimelineKey> keys_of(const Track& t) const { return keys.span().subspan(t.desc.first_key, t.desc.key_count); }
        StringView string(u32 offset) const;
    };

    // The definition a player plays: its cooked asset (copied out once, re-read on hot reload), or
    // one registered with define. Null when it does not resolve (warned once).
    const Def* resolve(const TimelinePlayer& p);
    // Registers (or replaces) the timeline played for `guid` from a CHNK_TIMELINE payload, ahead
    // of the asset system: tools preview unsaved edits through it, tests skip the cook.
    Result<void> define(asset::Guid guid, Span<const byte> payload);
    // Resolves a track's field path in `world` (Property / Activation); false when it does not.
    bool bind(World& world, Track& t, const Def& d);

private:
    Result<void> copy(Def& out, asset::Guid guid, Span<const byte> payload) const;
    asset::AssetSystem* assets_ = nullptr;
    Array<Def> defs_;
    Array<asset::Guid> warned_;
};

// Registers TimelinePlayer and TimelineSignal, sets the TimelineRegistry resource and adds
// "timeline_player" to FixedUpdate_Post. Idempotent (re-points the assets).
void install_timelines(World& world, asset::AssetSystem* assets);

// The system itself (exposed for tests and custom pipelines).
void timeline_player(Frame& f);

// Moves `e`'s playhead to `seconds` (clamped to the timeline) and applies the state tracks there
// without firing keys; the keys after it fire when it plays on. False when `e` has no resolvable
// TimelinePlayer.
bool timeline_scrub(World& world, Entity e, f32 seconds);

// Skips `e`'s timeline to its end (a skipped cutscene, kit::Cutscene): the event keys after the
// playhead fire (their TimelineSignal / SeqSignal, so gameplay that waits on a beat still hears
// it), each animation track's last key after the playhead starts its clip, audio keys stay
// silent, the state tracks apply at the end and the player finishes. False when `e` has no
// resolvable TimelinePlayer.
bool timeline_skip(Frame& f, Entity e);

// The eased fraction for `u` in [0, 1] between two keys (Step holds the first key: 0).
f32 timeline_eased(asset::TimelineEase e, f32 u);

// The subtitle line `e`'s player shows: false when none.
bool timeline_subtitle(World& world, Entity e, StringView& speaker, StringView& text);

}  // namespace mce::kit
