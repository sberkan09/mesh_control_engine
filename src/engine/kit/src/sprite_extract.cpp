#include "mce/kit/sprite_extract.h"

#include <cmath>

#include "mce/asset/formats.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/kit/effects.h"
#include "mce/kit/particles.h"
#include "mce/loc/loc.h"
#include "mce/log.h"
#include "mce/memory/frame_slots.h"
#include "mce/render/asset_upload.h"

namespace mce::kit {

namespace {

// One region covering the whole texture, one metre square: Sprite::size is the world size.
constexpr render::SpriteRegion kWholeTexture{0, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f};

render::SpriteSetId whole_texture_set(render::Renderer& r, u32 texture_key, rhi::Filter filter, const char* name) {
    render::SpriteSetDesc d;
    d.texture_key = texture_key;
    d.regions = Span<const render::SpriteRegion>(&kWholeTexture, 1);
    d.filter = filter;
    d.debug_name = name;
    return r.create_sprite_set(d);
}

}  // namespace

ResultV SpriteExtract::init(render::Renderer& renderer, asset::AssetSystem* assets) {
    renderer_ = &renderer;
    assets_ = assets;
    // A 1x1 white texture under a renderer-local key: solid quads take their color from the tint.
    white_key_ = renderer.textures().allocate_key();
    static constexpr u8 kWhite[4] = {255, 255, 255, 255};
    render::TextureMipUpload mip;
    mip.width = 1;
    mip.height = 1;
    mip.pixels = Span<const byte>(reinterpret_cast<const byte*>(kWhite), 4);
    render::TextureUpload up;
    up.mips = Span<const render::TextureMipUpload>(&mip, 1);
    up.debug_name = "kit.white";
    MCE_TRY(renderer.textures().upload(white_key_, 1, up));
    white_set_ = whole_texture_set(renderer, white_key_, rhi::Filter::Nearest, "kit.solid");
    // A key that is never uploaded resolves to the store's magenta checker.
    missing_set_ = whole_texture_set(renderer, renderer.textures().allocate_key(), rhi::Filter::Nearest, "kit.missing");
    return ok();
}

void SpriteExtract::release_handles() {
    for (TilemapEntry& e : tilemaps_) destroy_tilemap(e);
    tilemaps_.clear();
    if (assets_)
        for (const TextureEntry& t : textures_) {
            if (t.handle.generation != 0) assets_->release(t.handle);
            if (t.normal_handle.generation != 0) assets_->release(t.normal_handle);
        }
    if (renderer_)
        for (const TextureEntry& t : textures_)
            if (t.set.valid() && !(t.set == missing_set_)) renderer_->sprite_sets().destroy(t.set);
    textures_.clear();
    fonts_.clear();  // the renderer keeps font storage until shutdown
}

void SpriteExtract::shutdown() {
    if (!renderer_) return;
    release_handles();
    if (white_set_.valid()) renderer_->sprite_sets().destroy(white_set_);
    if (missing_set_.valid()) renderer_->sprite_sets().destroy(missing_set_);
    renderer_->textures().release(white_key_);
    white_set_ = {};
    missing_set_ = {};
    renderer_ = nullptr;
    assets_ = nullptr;
}

void SpriteExtract::reset(asset::AssetSystem* assets) {
    release_handles();
    assets_ = assets;
}

render::SpriteSetId SpriteExtract::sprite_set(asset::Guid guid, asset::Guid normal) {
    if (guid == asset::Guid{} && (normal == asset::Guid{} || !assets_)) return white_set_;
    for (const TextureEntry& t : textures_)
        if (t.guid == guid && t.normal == normal) return t.set;
    TextureEntry e;
    e.guid = guid;
    e.normal = normal;
    e.set = missing_set_;
    auto load = [&](asset::Guid g, asset::AnyHandle& out) -> asset::AssetKey {
        const asset::AssetKey key = assets_->find(g);
        if (key == asset::kInvalidAssetKey) {
            MCE_LOG_WARN("kit: texture {} is not in the mounted content", asset::to_string(g).view());
            return key;
        }
        Result<asset::AssetHandle<asset::Texture>> h = assets_->load_sync<asset::Texture>(key);
        if (!h) {
            MCE_LOG_WARN("kit: texture {} failed to load; drawing the missing-texture checker", asset::to_string(g).view());
            return asset::kInvalidAssetKey;
        }
        out = asset::AnyHandle(h.value());
        return key;
    };
    if (assets_) {
        // The color texture: an asset, or the white texture under a lit solid quad.
        u32 color_key = white_key_;
        bool color_ok = true;
        if (guid != asset::Guid{}) {
            const asset::AssetKey key = load(guid, e.handle);
            color_ok = key != asset::kInvalidAssetKey;
            color_key = key;
        }
        if (color_ok) {
            render::SpriteSetDesc d;
            d.texture_key = color_key;
            d.regions = Span<const render::SpriteRegion>(&kWholeTexture, 1);
            d.debug_name = "kit.sprite";
            if (guid != asset::Guid{}) {
                auto tv = asset::TextureView::read(assets_->resolve(asset::AssetHandle<asset::Texture>{e.handle.index, e.handle.generation}));
                if (tv && (tv.value().header->flags & asset::TextureFlags::LinearFilter) != 0) d.filter = rhi::Filter::Linear;
            }
            if (normal != asset::Guid{}) {
                const asset::AssetKey nkey = load(normal, e.normal_handle);
                if (nkey != asset::kInvalidAssetKey) {
                    d.lit = true;
                    d.normal_key = nkey;
                    d.debug_name = "kit.sprite.lit";
                }
            }
            e.set = renderer_->create_sprite_set(d);
            e.regions.push_back(kWholeTexture);
        }
    }
    const render::SpriteSetId set = e.set;
    textures_.push_back(static_cast<TextureEntry&&>(e));
    return set;
}

u32 SpriteExtract::region_index(render::SpriteSetId set, const math::vec4& uv) {
    if (uv.x == 0.0f && uv.y == 0.0f && uv.z == 1.0f && uv.w == 1.0f) return 0;
    if (!renderer_ || set == white_set_ || set == missing_set_) return 0;
    for (TextureEntry& t : textures_) {
        if (!(t.set == set) || t.regions.empty()) continue;
        for (u32 i = 1; i < t.regions.size(); ++i) {
            const render::SpriteRegion& r = t.regions[i];
            if (r.u0 == uv.x && r.v0 == uv.y && r.u1 == uv.z && r.v1 == uv.w) return i;
        }
        constexpr u32 kMaxRegions = 4096;
        if (t.regions.size() >= kMaxRegions) return 0;
        render::SpriteRegion r = kWholeTexture;
        r.u0 = uv.x;
        r.v0 = uv.y;
        r.u1 = uv.z;
        r.v1 = uv.w;
        t.regions.push_back(r);
        render::SpriteSets& sets = renderer_->sprite_sets();
        render::SpriteSetDesc d;
        d.texture_key = sets.texture_key(set);
        d.filter = sets.filter(set);
        d.lit = sets.lit(set);
        d.normal_key = sets.normal_key(set);
        d.regions = t.regions.span();
        d.debug_name = d.lit ? "kit.sprite.lit" : "kit.sprite";
        sets.update(set, d);
        return static_cast<u32>(t.regions.size() - 1);
    }
    return 0;
}

const render::Font* SpriteExtract::font(asset::Guid guid) {
    if (guid == asset::Guid{}) return nullptr;
    for (const FontEntry& f : fonts_)
        if (f.guid == guid) return f.ok ? &f.font : nullptr;
    FontEntry e;
    e.guid = guid;
    if (assets_) {
        const asset::AssetKey key = assets_->find(guid);
        Result<asset::AssetHandle<asset::Font>> h =
            key != asset::kInvalidAssetKey ? assets_->load_sync<asset::Font>(key) : Result<asset::AssetHandle<asset::Font>>(Error(Errc::NotFound, "kit.font"));
        if (h) {
            Result<render::Font> f = render::load_cooked_font(*renderer_, assets_->resolve(h.value()), "kit.font");
            assets_->release(asset::AnyHandle(h.value()));  // the renderer copied what it needs
            if (f) {
                e.font = f.value();
                e.ok = true;
            }
        }
        if (!e.ok) MCE_LOG_WARN("kit: font {} cannot be loaded; its labels are skipped", asset::to_string(guid).view());
    }
    fonts_.push_back(e);
    return e.ok ? &fonts_.back().font : nullptr;
}

// Where an entity is drawn: its WorldTransform when the hierarchy wrote one (ChildOf scenes,
// ADR-0030 §5), else its LocalTransform. Hierarchy-less scenes (every scene before M6) take only
// the first query of each pair, in the same order as before.
namespace {

struct Placement {
    math::vec3 position;
    math::quat rotation;
    math::vec3 scale;
};
inline Placement placement(const LocalTransform& t) { return {t.position, t.rotation, t.scale}; }
inline Placement placement(const WorldTransform& t) { return {t.position, t.rotation, t.scale}; }

// Parallax layers draw shifted by camera × (1 − factor) (FR-R2D-050). Entities without one take
// the first two queries, in the same order as before parallax existed.
Placement shifted(Placement p, const Parallax& px, math::vec2 cam) {
    p.position.x += cam.x * (1.0f - px.factor.x);
    p.position.y += cam.y * (1.0f - px.factor.y);
    return p;
}

template <typename C, typename Fn>
void each_placed(World& world, math::vec2 cam, Fn&& fn) {
    world.query<const LocalTransform, const C>().template without<WorldTransform, Parallax>().each(
        [&](const LocalTransform& t, const C& c) { fn(placement(t), c, nullptr); });
    world.query<const WorldTransform, const C>().template without<Parallax>().each(
        [&](const WorldTransform& t, const C& c) { fn(placement(t), c, nullptr); });
    world.query<const LocalTransform, const C, const Parallax>().template without<WorldTransform>().each(
        [&](const LocalTransform& t, const C& c, const Parallax& p) { fn(shifted(placement(t), p, cam), c, &p); });
    world.query<const WorldTransform, const C, const Parallax>().each(
        [&](const WorldTransform& t, const C& c, const Parallax& p) { fn(shifted(placement(t), p, cam), c, &p); });
}

}  // namespace

void SpriteExtract::collect_lights(World& world, render::SpriteBatch& batch) {
    render::Lights2D lights;
    bool any_ambient = false;
    math::Color ambient{0, 0, 0, 1};
    const math::aabb2& view = batch.view().world_bounds;
    auto add = [&](const Placement& t, const Light2D& l) {
        if (l.kind == Light2DKind::Ambient) {
            any_ambient = true;
            ambient.r += l.color.r * l.intensity;
            ambient.g += l.color.g * l.intensity;
            ambient.b += l.color.b * l.intensity;
            return;
        }
        const math::vec2 p{t.position.x, t.position.y};
        const math::aabb2 reach{p - math::vec2(l.radius), p + math::vec2(l.radius)};
        if (l.intensity <= 0.0f || !reach.overlaps(view)) return;
        render::PointLight2D pl;
        pl.position = p;
        pl.height = l.height;
        pl.radius = l.radius;
        pl.color = l.color;
        pl.intensity = l.intensity;
        lights.add(pl);  // past 16, the rest are dropped (ECS order)
    };
    world.query<const LocalTransform, const Light2D>().without<WorldTransform>().each(
        [&](const LocalTransform& t, const Light2D& l) { add(placement(t), l); });
    world.query<const WorldTransform, const Light2D>().each([&](const WorldTransform& t, const Light2D& l) { add(placement(t), l); });
    if (any_ambient) lights.ambient = ambient;
    batch.set_lights(lights);
}

void SpriteExtract::extract(World& world, render::SpriteBatch& batch) {
    const math::vec2 cam = batch.view().center;
    collect_lights(world, batch);
    each_placed<Sprite>(world, cam, [&](const Placement& t, const Sprite& s, const Parallax* px) {
        render::SpriteInstance inst;
        inst.position = {t.position.x, t.position.y};
        inst.rotation = angle_z(t.rotation);
        inst.scale = {s.size.x * t.scale.x, s.size.y * t.scale.y};
        inst.pivot = s.pivot;
        inst.tint = s.tint;
        inst.set = sprite_set(s.texture.guid, s.normal_map.guid);
        inst.region = region_index(inst.set, s.region);
        inst.sort_layer = s.layer;
        inst.sort_order = s.order;
        inst.flip_x = s.flip_x;
        inst.flip_y = s.flip_y;
        if (!px || !px->repeat_x) {
            batch.add(inst);
            return;
        }
        // Endless background: copies one width apart across the view, starting left of it.
        const f32 w = std::fabs(inst.scale.x);
        if (w <= 0.0f) return;
        const math::aabb2& view = batch.view().world_bounds;
        const f32 left = inst.position.x - w * inst.pivot.x;  // the authored copy's left edge
        const f32 k0 = std::floor((view.min.x - left) / w);
        const f32 k1 = std::ceil((view.max.x - left) / w);
        for (f32 k = k0; k < k1 && k - k0 < 256.0f; k += 1.0f) {
            render::SpriteInstance copy = inst;
            copy.position.x = inst.position.x + k * w;
            batch.add(copy);
        }
    });

    const loc::Localization* l10n = world.try_resource<loc::Localization>();
    each_placed<TextLabel>(world, cam, [&](const Placement& t, const TextLabel& l, const Parallax*) {
        const render::Font* f = font(l.font.guid);
        StringView text = l.text.view();
        if (!l.loc_key.empty() && l10n) text = l10n->text(l.loc_key.view());
        if (!f || text.empty() || f->face.metrics().pixel_height <= 0.0f) return;
        const f32 upp = l.height * t.scale.y / f->face.metrics().pixel_height;
        render::TextLayout layout(mem::frame());
        render::TextLayoutDesc desc;
        desc.align = static_cast<render::TextAlign>(l.align);
        if (l.wrap_width > 0.0f && upp > 0.0f) desc.max_width = l.wrap_width * t.scale.x / upp;
        render::layout_text(f->face, text, desc, layout);
        render::TextPlacement p;
        p.units_per_pixel = upp;
        p.y_down = false;
        p.tint = l.color;
        p.sort_layer = l.layer;
        p.sort_order = l.order;
        const f32 box = desc.max_width > 0.0f ? desc.max_width : layout.width;  // the lines align inside it
        f32 x = t.position.x;
        if (l.align == TextAlign::Center) x -= 0.5f * box * upp;
        else if (l.align == TextAlign::Right) x -= box * upp;
        p.origin = {x, t.position.y + 0.5f * layout.height * upp};
        batch.add_text(*f, layout, p);
    });

    // Particles go through the same batch as sprites (FR-R2D-070).
    if (const ParticleSystem* ps = world.try_resource<ParticleSystem>())
        ps->draw(batch, [](void* self, asset::Guid tex) { return static_cast<SpriteExtract*>(self)->sprite_set(tex); }, this);

    for (TilemapEntry& e : tilemaps_) e.used = false;
    each_placed<TilemapLayer>(world, cam, [&](const Placement& t, const TilemapLayer& l, const Parallax* px) {
        // A parallax layer stays baked where it was authored; the shift is a draw-time offset.
        const math::vec2 at{t.position.x, t.position.y};
        const math::vec2 shift = px ? math::vec2{cam.x * (1.0f - px->factor.x), cam.y * (1.0f - px->factor.y)} : math::vec2{};
        draw_tilemap(l, at - shift, shift, batch);
    });
    // Layers that went away this frame free their baked buffers.
    for (usize i = tilemaps_.size(); i-- > 0;) {
        if (tilemaps_[i].used) continue;
        destroy_tilemap(tilemaps_[i]);
        tilemaps_.erase(i);
    }
}

void SpriteExtract::destroy_tilemap(TilemapEntry& e) {
    if (renderer_ && e.id.valid()) renderer_->destroy_tilemap(e.id);
    if (renderer_ && e.set.valid()) renderer_->sprite_sets().destroy(e.set);
    if (assets_ && e.handle.generation != 0) assets_->release(e.handle);
    e = TilemapEntry{};
}

void SpriteExtract::draw_tilemap(const TilemapLayer& layer, math::vec2 origin, math::vec2 offset, render::SpriteBatch& batch) {
    const asset::Guid guid = layer.map.guid;
    if (guid == asset::Guid{} || !assets_ || !renderer_) return;
    // The n-th layer of one map this frame takes the n-th entry (two layers may share a map).
    TilemapEntry* e = nullptr;
    for (TilemapEntry& t : tilemaps_)
        if (!t.used && t.guid == guid) {
            e = &t;
            break;
        }
    if (!e) {
        tilemaps_.push_back(TilemapEntry{});
        e = &tilemaps_.back();
        e->guid = guid;
    }
    e->used = true;
    const bool stale = e->handle.generation == 0 || !assets_->is_alive(e->handle);  // first sight, or hot-reloaded
    const bool moved = !(e->origin.x == origin.x && e->origin.y == origin.y) || e->order != layer.order ||
                       e->layer != layer.layer || !(e->tint.r == layer.tint.r && e->tint.g == layer.tint.g && e->tint.b == layer.tint.b && e->tint.a == layer.tint.a);
    if (stale || moved || !e->id.valid()) {
        if (stale) {
            if (e->handle.generation != 0) assets_->release(e->handle);
            e->handle = {};
            const asset::AssetKey key = assets_->find(guid);
            Result<asset::AssetHandle<asset::Tilemap>> h =
                key == asset::kInvalidAssetKey ? Result<asset::AssetHandle<asset::Tilemap>>(Error(Errc::NotFound, "kit.tilemap"))
                                               : assets_->load_sync<asset::Tilemap>(key);
            if (!h) {
                MCE_LOG_WARN("kit: tilemap {} cannot be loaded; the layer is skipped", asset::to_string(guid).view());
                return;
            }
            e->handle = asset::AnyHandle(h.value());
        }
        Result<asset::TilemapView> view = asset::TilemapView::read(assets_->resolve(asset::AssetHandle<asset::Tilemap>{e->handle.index, e->handle.generation}));
        if (!view) return;
        const asset::TilemapHeader& th = *view.value().header;
        // The tileset cut into columns x rows cells, top row first (cell n = tile n + 1).
        Array<render::SpriteRegion> cells;
        for (u32 r = 0; r < th.rows; ++r)
            for (u32 c = 0; c < th.columns; ++c) {
                render::SpriteRegion reg;
                reg.u0 = static_cast<f32>(c) / static_cast<f32>(th.columns);
                reg.u1 = static_cast<f32>(c + 1) / static_cast<f32>(th.columns);
                reg.v0 = static_cast<f32>(r) / static_cast<f32>(th.rows);
                reg.v1 = static_cast<f32>(r + 1) / static_cast<f32>(th.rows);
                cells.push_back(reg);
            }
        const render::SpriteSetId whole = sprite_set(th.tileset_guid());  // loads and holds the texture
        render::SpriteSetDesc sd;
        sd.texture_key = renderer_->sprite_sets().texture_key(whole);
        sd.regions = cells.span();
        sd.debug_name = "kit.tileset";
        if (e->set.valid()) renderer_->sprite_sets().update(e->set, sd);
        else e->set = renderer_->create_sprite_set(sd);
        render::TilemapDesc td;
        td.set = e->set;
        td.width = th.width;
        td.height = th.height;
        td.tile_size = th.tile_size;
        td.origin = origin;
        td.tiles = view.value().tiles;
        td.tint = layer.tint;
        td.sort_layer = layer.layer;
        td.sort_order = layer.order;
        if (e->id.valid()) {
            if (!renderer_->update_tilemap(e->id, td)) return;
        } else {
            Result<render::TilemapId> id = renderer_->create_tilemap(td);
            if (!id) return;
            e->id = id.value();
        }
        e->origin = origin;
        e->tint = layer.tint;
        e->order = layer.order;
        e->layer = layer.layer;
    }
    renderer_->add_tilemap(batch, e->id, offset);
}

bool SpriteExtract::post_2d(World& world, render::Post2D& out) {
    out = render::Post2D{};
    bool found = false;
    world.query<const Camera2DComponent, const Post2DComponent>().each([&](const Camera2DComponent& c, const Post2DComponent& p) {
        if (found || !c.primary) return;
        found = true;
        out.vignette = p.vignette;
        out.vignette_intensity = p.vignette_intensity;
        out.vignette_radius = p.vignette_radius;
        out.vignette_softness = p.vignette_softness;
        out.chromatic_aberration = p.chromatic_aberration;
        out.aberration = p.aberration;
        out.lut_strength = p.lut_strength;
        if (p.color_grade && p.lut.guid != asset::Guid{}) {
            const render::SpriteSetId set = sprite_set(p.lut.guid);  // loads and holds the texture
            if (set.valid() && !(set == missing_set_)) {
                out.color_grade = true;
                out.lut_key = renderer_->sprite_sets().texture_key(set);
            }
        }
    });
    return found && out.any();
}

namespace {

// `shake` (may be null): the camera's CameraShake2D offset this tick.
void camera_from(const LocalTransform& t, const Camera2DComponent& c, const CameraShake2D* shake, render::Camera2D& out,
                 f32& pixels_per_meter, math::Color* clear_color) {
    out.position = {t.position.x, t.position.y};
    out.zoom = c.zoom;
    out.rotation = angle_z(t.rotation);
    if (shake) {
        out.position = out.position + shake->offset;
        out.rotation += shake->offset_rotation;
    }
    out.pixel_snap = c.pixel_snap;
    out.fit_width = c.fit_width;
    out.fit_height = c.fit_height;
    pixels_per_meter = c.pixels_per_meter;
    if (clear_color) *clear_color = c.clear_color;
}

}  // namespace

bool primary_camera(World& world, render::Camera2D& out, f32& pixels_per_meter, math::Color* clear_color) {
    out = render::Camera2D{};
    pixels_per_meter = 48.0f;
    bool found = false;
    world.query<const LocalTransform, const Camera2DComponent>().each([&](Entity e, const LocalTransform& t, const Camera2DComponent& c) {
        if (found || !c.primary) return;
        found = true;
        camera_from(t, c, world.try_get<CameraShake2D>(e), out, pixels_per_meter, clear_color);
    });
    return found;
}

bool camera_2d_of(World& world, Entity e, render::Camera2D& out, f32& pixels_per_meter, math::Color* clear_color) {
    if (!world.alive(e)) return false;
    const LocalTransform* t = world.try_get<LocalTransform>(e);
    const Camera2DComponent* c = world.try_get<Camera2DComponent>(e);
    if (!t || !c) return false;
    camera_from(*t, *c, world.try_get<CameraShake2D>(e), out, pixels_per_meter, clear_color);
    return true;
}

}  // namespace mce::kit
