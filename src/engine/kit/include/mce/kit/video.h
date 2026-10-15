#pragma once
// Video (M-CINEMA 5, ADR-0042): a VideoPlayer plays a cooked .mpg (asset::Video) on its entity's
// Sprite, or over the whole screen, with its sound through the mixer.
//
//   kit::install_video(world, &assets);   // registry + "kit_video" (install_stock does it)
//   ... an entity with Sprite + VideoPlayer{video = guid of intro.mpg}: the sprite shows the film
//   ... or VideoPlayer{video = ..., fullscreen = true}: the player draws it over the scene (fitted
//       to the screen, black bars on the sides that do not match), under the UI and subtitles
//
// The sim side is the playhead: "kit_video" (FixedUpdate_Post, before the timeline player, running
// through cutscene holds)
// advances `time` by dt × speed while `playing`, wraps it with `loop`, and at the end of a
// non-looping video stops with `finished` set. A timeline video track drives the same fields (it
// sets `video`, `time` and `playing` while its key's range plays), so a skipped or scrubbed
// cutscene shows the right frame. Everything is component state: replays and the state hash see
// the playhead, never decoded pixels.
//
// The sound is the video's PCM (decoded at cook) played as one clip from the playhead: a start, a
// seek, a wrap or a jump of more than a tenth of a second (re)starts it there; stopping stops it.
// The render side (VideoTextures) decodes pictures: each frame it steps one decoder per shown
// VideoPlayer to its `time` and uploads the picture when it changed.

#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/audio/audio.h"
#include "mce/containers/array.h"
#include "mce/ecs/world.h"
#include "mce/refl/macros.h"
#include "mce/render/renderer.h"
#include "mce/render/video.h"

namespace mce {
class Frame;
}

namespace mce::kit {

// Plays a video. With a Sprite on the entity the film replaces the sprite's texture (the sprite's
// size, tint, pivot and layer still apply); with `fullscreen` the player draws it over the screen.
struct VideoPlayer {
    asset::AssetRef<asset::Video> video;
    f32 time = 0.0f;      // seconds into the video
    f32 speed = 1.0f;     // playback rate (the sound plays only at 1)
    f32 gain_db = 0.0f;   // the sound's gain
    bool playing = true;  // false pauses; the engine clears it when a non-looping video ends
    bool loop = false;
    bool fullscreen = false;
    bool sound = true;
    bool finished = false;  // runtime: a non-looping video reached its end
    bool restart = false;   // set to play from the start (the engine clears it)
    u8 pad_[2] = {};
    f32 duration = 0.0f;    // runtime: the video's length in seconds (0 until it is loaded)
};
MCE_COMPONENT(VideoPlayer, (video, Help{"the .mpg to play"}), (time, Save, Help{"seconds into the video"}),
              (speed, Range{0, 4}, Help{"playback rate; the sound plays only at 1"}), (gain_db, Range{-60, 12}),
              (playing, Save, Help{"false pauses; cleared when a non-looping video ends"}),
              (loop, Help{"wrap at the end and play again"}),
              (fullscreen, Help{"draw over the whole screen (under the UI) instead of on the entity's Sprite"}),
              (sound, Help{"play the video's sound"}), (finished, Save, ReadOnly),
              (restart, Help{"play again from the start (the engine clears it)"}), (duration, ReadOnly, Transient))

// The sim side's view of the cooked videos: their lengths, and the sound clips registered with the
// mixer (one per video, held while the registry lives). A world resource; install_video sets it.
class VideoRegistry {
public:
    VideoRegistry() = default;
    explicit VideoRegistry(asset::AssetSystem* assets) : assets_(assets) {}
    VideoRegistry(VideoRegistry&& o) noexcept;
    VideoRegistry& operator=(VideoRegistry&& o) noexcept;
    VideoRegistry(const VideoRegistry&) = delete;
    VideoRegistry& operator=(const VideoRegistry&) = delete;
    ~VideoRegistry();
    void set_assets(asset::AssetSystem* assets) { assets_ = assets; }

    struct Def {
        asset::Guid guid;
        asset::AnyHandle handle;  // held: the sound's PCM is the mapped chunk
        u32 generation = 0;
        asset::VideoView view;
        audio::ClipHandle clip;   // registered on first play with sound
        bool ok = false;
    };
    // The cooked video for `guid` (loaded and held on first use); null when it cannot load.
    const Def* resolve(asset::Guid guid);
    // The sound clip for `def`, registered with `device` on first use (invalid when silent).
    audio::ClipHandle clip(const Def& def, audio::AudioDevice& device);

    // Sound voices, one per playing VideoPlayer (kit_video keeps them in step).
    struct Voice {
        Entity entity;
        asset::Guid video;
        audio::AudioHandle handle;
        f32 expected = 0.0f;  // where the sound should be next tick if nothing jumps
    };
    Array<Voice>& voices() { return voices_; }

private:
    asset::AssetSystem* assets_ = nullptr;
    Array<Def> defs_;
    Array<Voice> voices_;
    Array<asset::Guid> warned_;
};

// Registers VideoPlayer, the registry and "kit_video". Idempotent (a second call updates the
// asset system); install_stock calls it.
void install_video(World& world, asset::AssetSystem* assets);
void kit_video(Frame& f);

// Render side: one decoder and one texture per shown VideoPlayer, stepped to its `time`.
//
//   kit::VideoTextures videos;  videos.init(renderer, &assets);
//   extract.set_videos(&videos);         // SpriteExtract draws a VideoPlayer's Sprite with its film
//   each frame: videos.update(world);    // before the extracts
class VideoTextures {
public:
    VideoTextures() = default;
    VideoTextures(const VideoTextures&) = delete;
    VideoTextures& operator=(const VideoTextures&) = delete;
    ~VideoTextures() { shutdown(); }

    ResultV init(render::Renderer& renderer, asset::AssetSystem* assets);
    void shutdown();
    // Steps every VideoPlayer's picture to its `time`, uploads the pictures that changed and drops
    // the decoders of entities that lost their player.
    void update(World& world);
    // The sprite set showing `e`'s video (invalid when it has none yet).
    render::SpriteSetId sprite_set(Entity e) const;
    // The first fullscreen VideoPlayer's set and picture aspect (width / height); false when none.
    bool fullscreen(render::SpriteSetId& set, f32& aspect) const;
    // Pictures decoded by the last update() and since init (budget checks and tests).
    u32 decoded_last_update() const { return decoded_last_; }
    u32 live() const { return static_cast<u32>(entries_.size()); }

private:
    struct Entry {
        Entity entity;
        asset::Guid guid;
        asset::AnyHandle handle;
        u32 generation = 0;
        mem::UniquePtr<render::VideoDecoder> decoder;
        u32 texture_key = 0;
        rhi::TextureHandle texture;
        render::SpriteSetId set;
        bool fullscreen = false;
        bool seen = false;
        bool shown = false;  // a picture was uploaded
    };
    void drop(Entry& e);
    render::Renderer* renderer_ = nullptr;
    asset::AssetSystem* assets_ = nullptr;
    Array<Entry> entries_;
    u32 decoded_last_ = 0;
};

}  // namespace mce::kit
