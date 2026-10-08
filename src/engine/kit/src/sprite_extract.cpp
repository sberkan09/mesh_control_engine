#include "mce/kit/sprite_extract.h"

#include "mce/ecs/transform.h"
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
    if (assets_)
        for (const TextureEntry& t : textures_)
            if (t.handle.generation != 0) assets_->release(t.handle);
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

render::SpriteSetId SpriteExtract::sprite_set(asset::Guid guid) {
    if (guid == asset::Guid{}) return white_set_;
    for (const TextureEntry& t : textures_)
        if (t.guid == guid) return t.set;
    TextureEntry e;
    e.guid = guid;
    e.set = missing_set_;
    if (assets_) {
        const asset::AssetKey key = assets_->find(guid);
        if (key != asset::kInvalidAssetKey) {
            Result<asset::AssetHandle<asset::Texture>> h = assets_->load_sync<asset::Texture>(key);
            if (h) {
                e.handle = asset::AnyHandle(h.value());
                e.set = whole_texture_set(*renderer_, key, rhi::Filter::Nearest, "kit.sprite");
            } else {
                MCE_LOG_WARN("kit: texture {} failed to load; drawing the missing-texture checker",
                             asset::to_string(guid).view());
            }
        } else {
            MCE_LOG_WARN("kit: texture {} is not in the mounted content", asset::to_string(guid).view());
        }
    }
    textures_.push_back(e);
    return e.set;
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

void SpriteExtract::extract(World& world, render::SpriteBatch& batch) {
    world.query<const LocalTransform, const Sprite>().each([&](const LocalTransform& t, const Sprite& s) {
        render::SpriteInstance inst;
        inst.position = {t.position.x, t.position.y};
        inst.rotation = angle_z(t.rotation);
        inst.scale = {s.size.x * t.scale.x, s.size.y * t.scale.y};
        inst.pivot = s.pivot;
        inst.tint = s.tint;
        inst.set = sprite_set(s.texture.guid);
        inst.region = 0;
        inst.sort_layer = s.layer;
        inst.sort_order = s.order;
        inst.flip_x = s.flip_x;
        inst.flip_y = s.flip_y;
        batch.add(inst);
    });

    world.query<const LocalTransform, const TextLabel>().each([&](const LocalTransform& t, const TextLabel& l) {
        const render::Font* f = font(l.font.guid);
        if (!f || l.text.view().empty() || f->face.metrics().pixel_height <= 0.0f) return;
        render::TextLayout layout(mem::frame());
        render::TextLayoutDesc desc;
        render::layout_text(f->face, l.text.view(), desc, layout);
        const f32 upp = l.height * t.scale.y / f->face.metrics().pixel_height;
        render::TextPlacement p;
        p.units_per_pixel = upp;
        p.y_down = false;
        p.tint = l.color;
        p.sort_layer = l.layer;
        p.sort_order = l.order;
        f32 x = t.position.x;
        if (l.align == TextAlign::Center) x -= 0.5f * layout.width * upp;
        else if (l.align == TextAlign::Right) x -= layout.width * upp;
        p.origin = {x, t.position.y + 0.5f * layout.height * upp};
        batch.add_text(*f, layout, p);
    });
}

bool primary_camera(World& world, render::Camera2D& out, f32& pixels_per_meter, math::Color* clear_color) {
    out = render::Camera2D{};
    pixels_per_meter = 48.0f;
    bool found = false;
    world.query<const LocalTransform, const Camera2DComponent>().each([&](const LocalTransform& t, const Camera2DComponent& c) {
        if (found || !c.primary) return;
        found = true;
        out.position = {t.position.x, t.position.y};
        out.zoom = c.zoom;
        out.rotation = angle_z(t.rotation);
        out.pixel_snap = c.pixel_snap;
        pixels_per_meter = c.pixels_per_meter;
        if (clear_color) *clear_color = c.clear_color;
    });
    return found;
}

}  // namespace mce::kit
