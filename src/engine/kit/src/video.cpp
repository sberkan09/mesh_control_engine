#include "mce/kit/video.h"

#include <cmath>

#include "mce/audio/components.h"
#include "mce/log.h"

namespace mce::kit {

namespace {

constexpr f32 kResync = 0.1f;  // seconds the playhead may jump before the sound restarts there

constexpr render::SpriteRegion kWholePicture{0, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f};

}  // namespace

// ------------------------------------------------------------------ registry

VideoRegistry::VideoRegistry(VideoRegistry&& o) noexcept
    : assets_(o.assets_), defs_(static_cast<Array<Def>&&>(o.defs_)), voices_(static_cast<Array<Voice>&&>(o.voices_)),
      warned_(static_cast<Array<asset::Guid>&&>(o.warned_)) {
    o.assets_ = nullptr;
    o.defs_.clear();
}

VideoRegistry& VideoRegistry::operator=(VideoRegistry&& o) noexcept {
    if (this == &o) return *this;
    this->~VideoRegistry();
    new (this) VideoRegistry(static_cast<VideoRegistry&&>(o));
    return *this;
}

VideoRegistry::~VideoRegistry() {
    // The mixer's clips point into the held chunks: they are released with the device (the host
    // shuts its audio down before it unmounts the content), so only the asset references go here.
    if (assets_)
        for (Def& d : defs_)
            if (d.ok) assets_->release(d.handle);
    defs_.clear();
}

const VideoRegistry::Def* VideoRegistry::resolve(asset::Guid guid) {
    if (guid == asset::Guid{}) return nullptr;
    for (const Def& d : defs_)
        if (d.guid == guid) return d.ok ? &d : nullptr;
    Def d;
    d.guid = guid;
    if (assets_) {
        const asset::AssetKey key = assets_->find(guid);
        if (key != asset::kInvalidAssetKey) {
            if (Result<asset::AssetHandle<asset::Video>> h = assets_->load_sync<asset::Video>(key)) {
                if (Result<asset::VideoView> v = asset::VideoView::read(assets_->resolve(h.value()))) {
                    d.view = v.value();
                    d.handle = asset::AnyHandle(h.value());
                    d.generation = h.value().generation;
                    d.ok = true;
                } else {
                    assets_->release(asset::AnyHandle(h.value()));
                }
            }
        }
    }
    if (!d.ok) {
        bool warned = false;
        for (const asset::Guid& g : warned_) warned |= g == guid;
        if (!warned) {
            MCE_LOG_WARN("kit: video {} cannot be loaded; its players stay still", asset::to_string(guid).view());
            warned_.push_back(guid);
        }
    }
    defs_.push_back(static_cast<Def&&>(d));
    return defs_.back().ok ? &defs_.back() : nullptr;
}

audio::ClipHandle VideoRegistry::clip(const Def& def, audio::AudioDevice& device) {
    if (!def.ok || def.view.audio.empty() || def.view.header->audio_channels == 0) return {};
    Def& d = const_cast<Def&>(def);
    if (device.is_clip_registered(d.clip)) return d.clip;
    audio::ClipPayload p;
    p.frames = d.view.audio.data();
    p.frame_count = d.view.header->audio_frames;
    p.channels = d.view.header->audio_channels;
    p.sample_rate = audio::kSampleRate;
    if (Result<audio::ClipHandle> c = device.register_clip(p)) d.clip = c.value();
    return d.clip;
}

// ------------------------------------------------------------------ sim

namespace {

VideoRegistry::Voice* voice_of(VideoRegistry& reg, Entity e) {
    for (VideoRegistry::Voice& v : reg.voices())
        if (v.entity == e) return &v;
    return nullptr;
}

void stop_voice(VideoRegistry& reg, audio::AudioDevice* dev, usize i) {
    if (dev) dev->stop(reg.voices()[i].handle, 0.05f);
    reg.voices()[i] = reg.voices().back();
    reg.voices().pop_back();
}

}  // namespace

void kit_video(Frame& f) {
    World& w = f.world();
    VideoRegistry* reg = f.try_resource<VideoRegistry>();
    if (!reg) return;
    const audio::AudioRef* ar = f.try_resource<audio::AudioRef>();
    audio::AudioDevice* dev = ar ? ar->device : nullptr;

    f.query<VideoPlayer>().each([&](Entity e, VideoPlayer& p) {
        const VideoRegistry::Def* def = reg->resolve(p.video.guid);
        p.duration = def ? def->view.header->duration : 0.0f;
        if (p.restart) {
            p.restart = false;
            p.time = 0.0f;
            p.playing = true;
            p.finished = false;
        }
        bool wrapped = false;
        if (p.playing && def) {
            p.finished = false;
            p.time += f.dt * p.speed;
            if (p.duration > 0.0f && p.time >= p.duration) {
                if (p.loop) {
                    p.time = std::fmod(p.time, p.duration);
                    wrapped = true;
                } else {
                    p.time = p.duration;
                    p.playing = false;
                    p.finished = true;
                }
            }
        }

        // The sound follows the playhead.
        VideoRegistry::Voice* v = voice_of(*reg, e);
        // Wanted while playing at speed 1 with sound left to play (10 ms short of its end counts as played).
        const bool want = dev && def && p.playing && p.sound && p.speed == 1.0f && def->view.header->audio_channels != 0 &&
                          static_cast<u64>(static_cast<f64>(p.time) * audio::kSampleRate) + audio::kSampleRate / 100 <
                              def->view.header->audio_frames;
        if (v && (!want || !(v->video == def->guid) || wrapped || std::fabs(v->expected - p.time) > kResync || !dev->is_playing(v->handle))) {
            // Stopped, another video, a wrap, a jump or a voice the mixer dropped: restarted below when wanted.
            stop_voice(*reg, dev, static_cast<usize>(v - reg->voices().data()));
            v = nullptr;
        }
        if (want && !v) {
            const audio::ClipHandle clip = reg->clip(*def, *dev);
            if (clip.is_null()) return;
            audio::PlayParams params;
            params.gain_db = p.gain_db;
            params.bus = audio::Bus_Music;
            params.priority = 16;
            const u64 into = static_cast<u64>(static_cast<f64>(p.time) * audio::kSampleRate);
            const u64 now = dev->mix_clock();
            const audio::AudioHandle h = into > 0 && into <= now ? dev->play_at(clip, now - into, params) : dev->play(clip, params);
            reg->voices().push_back(VideoRegistry::Voice{e, def->guid, h, p.time});
            v = &reg->voices().back();
        }
        if (v) {
            v->expected = p.time + f.dt * p.speed;
            if (dev) dev->set_handle_gain(v->handle, p.gain_db);
        }
    });
    // Players that went away take their sound with them.
    for (usize i = 0; i < reg->voices().size();) {
        const Entity e = reg->voices()[i].entity;
        if (!w.alive(e) || !w.try_get<VideoPlayer>(e)) stop_voice(*reg, dev, i);
        else ++i;
    }
}

void install_video(World& world, asset::AssetSystem* assets) {
    if (VideoRegistry* r = world.try_resource<VideoRegistry>()) {
        r->set_assets(assets);
        return;
    }
    world.register_components<VideoPlayer>();
    world.set_resource(VideoRegistry(assets));
    world.add_system(Phase::FixedUpdate_Post, "kit_video", &kit_video);
    world.run_during_cutscenes(Phase::FixedUpdate_Post, "kit_video");
}

// ------------------------------------------------------------------ render side

ResultV VideoTextures::init(render::Renderer& renderer, asset::AssetSystem* assets) {
    shutdown();
    renderer_ = &renderer;
    assets_ = assets;
    return {};
}

void VideoTextures::drop(Entry& e) {
    e.decoder.reset();
    if (renderer_) {
        if (e.set.valid()) renderer_->sprite_sets().destroy(e.set);
        if (e.texture_key) renderer_->textures().release(e.texture_key);
    }
    if (assets_ && e.handle.generation != 0) assets_->release(e.handle);
    e = Entry{};
}

void VideoTextures::shutdown() {
    for (Entry& e : entries_) drop(e);
    entries_.clear();
    renderer_ = nullptr;
    assets_ = nullptr;
    decoded_last_ = 0;
}

void VideoTextures::update(World& world) {
    decoded_last_ = 0;
    if (!renderer_ || !assets_) return;
    for (Entry& e : entries_) e.seen = false;
    world.query<const VideoPlayer>().each([&](Entity ent, const VideoPlayer& p) {
        Entry* en = nullptr;
        for (Entry& e : entries_)
            if (e.entity == ent) en = &e;
        if (en && !(en->guid == p.video.guid)) {
            drop(*en);
            en->entity = ent;
        }
        if (!en) {
            entries_.push_back(Entry{});
            en = &entries_.back();
            en->entity = ent;
        }
        en->seen = true;
        en->fullscreen = p.fullscreen;
        if (!en->decoder) {
            en->guid = p.video.guid;
            const asset::AssetKey key = p.video.guid == asset::Guid{} ? asset::kInvalidAssetKey : assets_->find(p.video.guid);
            if (key == asset::kInvalidAssetKey) return;
            Result<asset::AssetHandle<asset::Video>> h = assets_->load_sync<asset::Video>(key);
            if (!h) return;
            en->handle = asset::AnyHandle(h.value());
            Result<asset::VideoView> v = asset::VideoView::read(assets_->resolve(h.value()));
            en->decoder = mem::make_unique<render::VideoDecoder>(mem::heap());
            if (!v || !en->decoder->open(v.value().mpeg)) {
                MCE_LOG_WARN("kit: video {} cannot be decoded", asset::to_string(p.video.guid).view());
                return;
            }
            en->decoder->set_duration(v.value().header->duration);
        }
        if (!en->decoder->is_open()) return;
        const u32 before = en->decoder->frames_decoded();
        if (!en->decoder->show(static_cast<f64>(p.time)) && en->shown) return;
        decoded_last_ += en->decoder->frames_decoded() - before;
        const render::VideoInfo& info = en->decoder->info();
        const Span<const u8> rgba = en->decoder->rgba();
        const Span<const byte> bytes(reinterpret_cast<const byte*>(rgba.data()), rgba.size());
        if (en->texture.is_null()) {
            // One texture per player, created with the first picture and updated in place after it.
            en->texture_key = renderer_->textures().allocate_key();
            const render::TextureMipUpload mip{info.width, info.height, bytes};
            render::TextureUpload up;
            up.mips = Span<const render::TextureMipUpload>(&mip, 1);
            up.debug_name = "kit.video";
            Result<rhi::TextureHandle> t = renderer_->textures().upload(en->texture_key, 1, up);
            if (!t) return;
            en->texture = t.value();
            render::SpriteSetDesc d;
            d.texture_key = en->texture_key;
            d.regions = Span<const render::SpriteRegion>(&kWholePicture, 1);
            d.filter = rhi::Filter::Linear;
            d.debug_name = "kit.video";
            en->set = renderer_->create_sprite_set(d);
        } else {
            rhi::TextureRegion r;
            r.width = info.width;
            r.height = info.height;
            renderer_->device().update_texture(en->texture, r, bytes);
        }
        en->shown = true;
    });
    for (usize i = 0; i < entries_.size();) {
        if (entries_[i].seen) {
            ++i;
            continue;
        }
        drop(entries_[i]);
        entries_[i] = static_cast<Entry&&>(entries_.back());
        entries_.pop_back();
    }
}

render::SpriteSetId VideoTextures::sprite_set(Entity e) const {
    for (const Entry& en : entries_)
        if (en.entity == e && en.shown) return en.set;
    return {};
}

bool VideoTextures::fullscreen(render::SpriteSetId& set, f32& aspect) const {
    for (const Entry& en : entries_) {
        if (!en.fullscreen || !en.shown || !en.decoder) continue;
        const render::VideoInfo& info = en.decoder->info();
        set = en.set;
        aspect = static_cast<f32>(info.width) / static_cast<f32>(info.height);
        return true;
    }
    return false;
}

}  // namespace mce::kit
