#include "mce/kit/sprite_anim.h"

#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/containers/array.h"
#include "mce/ecs/world.h"
#include "mce/kit/components.h"
#include "mce/log.h"
#include "mce/render/sprite_animation.h"

namespace mce::kit {

static_assert(sizeof(AnimatedSprite) == 160);  // padding-free: World::state_hash hashes component bytes
static_assert(sizeof(SpriteAnimEvent) == 16);

namespace {

// The loaded sheets, one reference per GUID (reloaded when the asset hot-reloads).
struct SpriteSheets {
    struct Entry {
        asset::Guid guid;
        asset::AnyHandle handle;
        bool failed = false;
    };
    asset::AssetSystem* assets = nullptr;
    Array<Entry> entries;

    bool view(asset::Guid guid, asset::SpriteSheetView& out) {
        if (!assets || guid == asset::Guid{}) return false;
        Entry* e = nullptr;
        for (Entry& x : entries)
            if (x.guid == guid) e = &x;
        if (!e) {
            entries.push_back(Entry{guid, {}, false});
            e = &entries.back();
        }
        if (e->handle.generation == 0 || !assets->is_alive(e->handle)) {
            if (e->failed) return false;
            if (e->handle.generation != 0) assets->release(e->handle);
            e->handle = {};
            const asset::AssetKey key = assets->find(guid);
            Result<asset::AssetHandle<asset::SpriteSheet>> h =
                key == asset::kInvalidAssetKey ? Result<asset::AssetHandle<asset::SpriteSheet>>(Error(Errc::NotFound, "kit.sheet"))
                                               : assets->load_sync<asset::SpriteSheet>(key);
            if (!h) {
                MCE_LOG_WARN("kit: sprite sheet {} cannot be loaded; its AnimatedSprites stay still", asset::to_string(guid).view());
                e->failed = true;
                return false;
            }
            e->handle = asset::AnyHandle(h.value());
        }
        Result<asset::SpriteSheetView> v =
            asset::SpriteSheetView::read(assets->resolve(asset::AssetHandle<asset::SpriteSheet>{e->handle.index, e->handle.generation}));
        if (!v) return false;
        out = v.value();
        return true;
    }
};

void kit_sprite_animation(Frame& f) {
    SpriteSheets& sheets = f.world().resource<SpriteSheets>();
    f.query<AnimatedSprite, Sprite>().each([&](Entity e, AnimatedSprite& a, Sprite& s) {
        asset::SpriteSheetView view;
        if (!sheets.view(a.sheet.guid, view)) return;
        const asset::SpriteSheetClip* c = view.find_clip(a.clip.view());
        if (!c) return;
        const StringView name = asset::SpriteSheetView::name_of(*c);

        SmallVector<render::AnimEvent, 8> events;
        for (u32 i = 0; i < c->event_count; ++i) {
            const asset::SpriteSheetEvent& ev = view.events[c->first_event + i];
            events.push_back(render::AnimEvent{ev.frame, ev.id});
        }
        render::SpriteClip clip;
        clip.frames = view.frames.subspan(c->first_frame, c->frame_count);
        clip.fps = c->fps;
        clip.mode = static_cast<render::PlayMode>(c->mode);
        clip.events = Span<const render::AnimEvent>(events.data(), events.size());

        auto fire = [&](const render::AnimEvent& ev) {
            ++a.events_fired;
            for (u32 i = 0; i < c->event_count; ++i)
                if (view.events[c->first_event + i].id == ev.id) {
                    a.last_event = asset::SpriteSheetView::name_of(view.events[c->first_event + i]);
                    break;
                }
            f.events().push(SpriteAnimEvent{e, ev.id, ev.frame, 0});
        };

        if (a.restart || a.playing_clip.view() != name) {
            a.restart = false;
            a.playing_clip = name;
            a.time = 0.0;
            a.frame = 0;
            a.finished = false;
            for (const render::AnimEvent& ev : clip.events)
                if (ev.frame == 0) fire(ev);
        } else if (a.playing && a.speed > 0.0f && !a.finished) {
            render::SpriteAnimState st{a.time, a.frame, a.finished};
            render::advance(clip, st, static_cast<f64>(f.dt) * static_cast<f64>(a.speed), fire);
            a.time = st.time;
            a.frame = st.frame;
            a.finished = st.finished;
        }

        if (view.header->texture_guid() != asset::Guid{}) s.texture.guid = view.header->texture_guid();
        const render::SpriteAnimState st{a.time, a.frame, a.finished};
        const u32 region = render::current_region(clip, st);
        if (region < view.regions.size()) {
            const asset::SpriteSheetRegion& r = view.regions[region];
            s.region = {r.u0, r.v0, r.u1, r.v1};
        }
    });
}

}  // namespace

void install_sprite_animation(World& world, asset::AssetSystem* assets) {
    if (SpriteSheets* s = world.try_resource<SpriteSheets>()) {
        s->assets = assets;
        return;
    }
    world.register_components<AnimatedSprite>();
    world.register_event<SpriteAnimEvent>(Phase::FixedUpdate_Post);
    SpriteSheets sheets;
    sheets.assets = assets;
    world.set_resource(static_cast<SpriteSheets&&>(sheets));
    world.add_system(Phase::FixedUpdate_Post, "kit_sprite_animation", &kit_sprite_animation);
}

}  // namespace mce::kit
