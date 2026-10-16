#include "mce/kit/render_textures.h"

#include "mce/log.h"

namespace mce::kit {

static_assert(sizeof(RenderTexture) == 56);
static_assert(sizeof(ShowRenderTexture) == 16);

namespace {
// The whole texture (record_views writes it upright, its top row at v = 0 like uploaded art).
constexpr render::SpriteRegion kWhole{0, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f};
}  // namespace

void RenderTextures::drop(Entry& e) {
    if (!renderer_) return;
    if (e.set.valid()) renderer_->sprite_sets().destroy(e.set);
    if (e.key != 0) renderer_->textures().release(e.key);  // destroys the texture (deferred past frames in flight)
    e.set = {};
    e.key = 0;
    e.texture = {};
}

void RenderTextures::release() {
    for (Entry& e : entries_) drop(e);
    entries_.clear();
    drawing_ = {};
}

void RenderTextures::shutdown() {
    release();
    renderer_ = nullptr;
}

void RenderTextures::sync(World& world, f32 frame_dt) {
    if (!renderer_ || !renderer_->initialized()) return;
    for (Entry& e : entries_) e.seen = false;
    world.query<const RenderTexture&>().each([&](Entity ent, const RenderTexture& rt) {
        if (!rt.enabled) return;
        const rhi::Extent2D size{rt.width < 1 ? 1u : rt.width > kMaxSize ? kMaxSize : rt.width,
                                 rt.height < 1 ? 1u : rt.height > kMaxSize ? kMaxSize : rt.height};
        Entry* en = find(ent);
        if (!en) {
            entries_.push_back({});
            en = &entries_.back();
            en->entity = ent;
        }
        en->seen = true;
        en->cleared = false;
        en->ui_image = rt.ui_image;
        if (en->texture.is_null() || !(en->size == size)) {
            drop(*en);
            rhi::TextureDesc d;
            d.size = size;
            d.format = rhi::TextureFormat::RGBA16F;
            d.usage = rhi::TextureUsage::SampledColorTarget;
            d.dimension = rhi::TextureDimension::Tex2DArray;
            d.array_layers = 1;
            d.debug_name = "kit.render_texture";
            en->texture = renderer_->device().create_texture(d);
            if (en->texture.is_null()) {
                MCE_LOG_WARN("kit: render texture {}x{} could not be created", size.width, size.height);
                return;
            }
            en->key = renderer_->textures().allocate_key();
            renderer_->textures().adopt(en->key, 1, en->texture);
            render::SpriteSetDesc sd;
            sd.texture_key = en->key;
            sd.regions = Span<const render::SpriteRegion>(&kWhole, 1);
            sd.filter = rhi::Filter::Linear;
            sd.debug_name = "kit.render_texture";
            en->set = renderer_->create_sprite_set(sd);
            en->size = size;
            en->carry = 0.0f;
            en->due = true;  // a new texture draws at once
            en->fresh = true;
        } else if (rt.update_hz > 0.0f) {
            en->carry += frame_dt > 0.0f ? frame_dt : 0.0f;
            const f32 period = 1.0f / rt.update_hz;
            en->due = en->carry >= period;
            if (en->due) en->carry = en->carry >= 2.0f * period ? 0.0f : en->carry - period;  // no catch-up burst
        } else {
            en->due = true;
        }
        en->update_hz = rt.update_hz;
    });
    for (usize i = 0; i < entries_.size();) {
        if (entries_[i].seen) {
            ++i;
            continue;
        }
        drop(entries_[i]);
        entries_[i] = entries_.back();
        entries_.pop_back();
    }
}

void RenderTextures::clear_new(rhi::CommandBuffer& cmd) {
    for (Entry& e : entries_) {
        if (!e.fresh || e.texture.is_null()) continue;
        rhi::PassAttachment c;
        c.texture = e.texture;
        c.clear.color[3] = 1.0f;
        rhi::RenderPassDesc p;
        p.colors = Span<const rhi::PassAttachment>(&c, 1);
        p.debug_name = "kit.render_texture_clear";
        cmd.begin_pass(p);
        cmd.end_pass();
        e.fresh = false;
        e.cleared = true;
    }
}

const RenderTextures::Entry* RenderTextures::find(Entity e) const {
    for (const Entry& en : entries_)
        if (en.entity == e) return &en;
    return nullptr;
}

RenderTextures::Entry* RenderTextures::find(Entity e) {
    for (Entry& en : entries_)
        if (en.entity == e) return &en;
    return nullptr;
}

u32 RenderTextures::texture_key(Entity source) const {
    if (source.is_null() || source == drawing_) return ~0u;
    const Entry* e = find(source);
    return e && e->key != 0 ? e->key : ~0u;
}

render::SpriteSetId RenderTextures::sprite_set(Entity source) const {
    if (source.is_null() || source == drawing_) return {};
    const Entry* e = find(source);
    return e ? e->set : render::SpriteSetId{};
}

rhi::Extent2D render_texture_size(World&, Entity target, void* user) {
    const RenderTextures* rts = static_cast<const RenderTextures*>(user);
    const RenderTextures::Entry* e = rts ? rts->find(target) : nullptr;
    return e && !e->texture.is_null() ? e->size : rhi::Extent2D{};
}

}  // namespace mce::kit
