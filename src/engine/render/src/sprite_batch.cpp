#include "mce/render/sprite_batch.h"

#include <cmath>

#include "mce/log.h"

namespace mce::render {

using namespace mce::math;

// ---- SpriteSets ---------------------------------------------------------------------------------

SpriteSetId SpriteSets::create(const SpriteSetDesc& desc) {
    u16 index;
    if (!free_.empty()) {
        index = free_.back();
        free_.pop_back();
    } else {
        MCE_ASSERT_MSG(sets_.size() < kMaxSets, "too many sprite sets (sort-key material field is 12 bits)");
        if (sets_.size() >= kMaxSets) return {};
        index = static_cast<u16>(sets_.size());
        sets_.push_back(Set{});
    }
    sets_[index].alive = true;
    update(SpriteSetId{index}, desc);
    return SpriteSetId{index};
}

void SpriteSets::update(SpriteSetId id, const SpriteSetDesc& desc) {
    MCE_ASSERT_MSG(valid(id), "SpriteSets::update: invalid id");
    if (!valid(id)) return;
    Set& s = sets_[id.value];
    s.texture_key = desc.texture_key;
    s.regions.clear();
    for (const SpriteRegion& r : desc.regions) s.regions.push_back(r);
    s.pixels_per_meter = desc.pixels_per_meter > 0.0f ? desc.pixels_per_meter : 48.0f;
    s.filter = desc.filter;
    s.coverage = desc.coverage;
    s.sdf = desc.coverage && desc.sdf;
    s.lit = desc.lit;
    s.normal_key = desc.normal_key;
}

void SpriteSets::destroy(SpriteSetId id) {
    if (!valid(id)) return;
    sets_[id.value] = Set{};
    free_.push_back(id.value);
}

bool SpriteSets::valid(SpriteSetId id) const { return id.value < sets_.size() && sets_[id.value].alive; }

const SpriteRegion& SpriteSets::region(SpriteSetId id, u32 index) const {
    if (!valid(id)) return fallback_;
    const Set& s = sets_[id.value];
    return index < s.regions.size() ? s.regions[index] : fallback_;
}

// ---- tilemap bake -------------------------------------------------------------------------------

ResultV bake_tilemap(const TilemapDesc& d, const SpriteSets& sets, TilemapBake& out) {
    out.vertices.clear();
    out.chunks.clear();
    if (!sets.valid(d.set)) return Error(Errc::InvalidArgument, "render.tilemap.set");
    if (usize(d.width) * d.height != d.tiles.size())
        return Error(Errc::InvalidArgument, "render.tilemap.size", {}, static_cast<u32>(d.tiles.size()));
    if (!(d.tile_size > 0.0f)) return Error(Errc::InvalidArgument, "render.tilemap.tile_size");
    out.set = d.set;
    out.sort_layer = d.sort_layer;
    out.sort_order = d.sort_order;
    out.blend = d.blend;

    const u32 color = pack_color_srgb(d.tint);
    const u32 flags = (sets.coverage(d.set) ? vertex_flags::kCoverage : 0u) | (sets.sdf(d.set) ? vertex_flags::kSdf : 0u) |
                      (sets.lit(d.set) ? vertex_flags::kLit : 0u);
    const u32 regions = sets.region_count(d.set);
    const u32 cx_count = (d.width + kTileChunkSize - 1) / kTileChunkSize;
    const u32 cy_count = (d.height + kTileChunkSize - 1) / kTileChunkSize;
    u32 bad_tiles = 0;
    for (u32 cy = 0; cy < cy_count; ++cy) {
        for (u32 cx = 0; cx < cx_count; ++cx) {
            TilemapBake::Chunk chunk;
            chunk.first_quad = out.quad_count();
            const u32 x_end = math::min(d.width, (cx + 1) * kTileChunkSize);
            const u32 y_end = math::min(d.height, (cy + 1) * kTileChunkSize);
            for (u32 y = cy * kTileChunkSize; y < y_end; ++y) {
                for (u32 x = cx * kTileChunkSize; x < x_end; ++x) {
                    const u32 tile = d.tiles[usize(y) * d.width + x];
                    if (tile == 0) continue;
                    if (tile > regions) ++bad_tiles;  // drawn with the fallback region
                    const SpriteRegion& r = sets.region(d.set, tile - 1);
                    const f32 x0 = d.origin.x + static_cast<f32>(x) * d.tile_size;
                    const f32 y0 = d.origin.y + static_cast<f32>(y) * d.tile_size;
                    const f32 x1 = x0 + d.tile_size, y1 = y0 + d.tile_size;
                    const u32 packed = pack_layer_flags(r.layer, flags);
                    out.vertices.push_back({x0, y0, r.u0, r.v1, color, packed});
                    out.vertices.push_back({x1, y0, r.u1, r.v1, color, packed});
                    out.vertices.push_back({x1, y1, r.u1, r.v0, color, packed});
                    out.vertices.push_back({x0, y1, r.u0, r.v0, color, packed});
                }
            }
            chunk.quad_count = out.quad_count() - chunk.first_quad;
            if (chunk.quad_count == 0) continue;
            const vec2 lo(d.origin.x + static_cast<f32>(cx * kTileChunkSize) * d.tile_size,
                          d.origin.y + static_cast<f32>(cy * kTileChunkSize) * d.tile_size);
            const vec2 hi(d.origin.x + static_cast<f32>(x_end) * d.tile_size, d.origin.y + static_cast<f32>(y_end) * d.tile_size);
            chunk.bounds = {lo, hi};
            out.chunks.push_back(chunk);
        }
    }
    if (bad_tiles) MCE_LOG_WARN("render: tilemap has {} tiles outside its {} regions", bad_tiles, regions);
    return ok();
}

// ---- SpriteBatch --------------------------------------------------------------------------------

SpriteBatch::SpriteBatch(const SpriteSets& sets, mem::IAllocator& frame)
    : sets_(sets),
      alloc_(frame),
      keys_(frame),
      items_(frame),
      quads_(frame),
      tilemaps_(frame),
      tile_args_(frame),
      vertices_(frame),
      args_(frame),
      groups_(frame),
      segments_(frame) {}

void SpriteBatch::begin(const View2D& view, PassId pass) {
    view_ = view;
    lights_ = Lights2D{};
    pass_ = pass;
    keys_.clear();
    items_.clear();
    quads_.clear();
    tilemaps_.clear();
    tile_args_.clear();
    vertices_.clear();
    args_.clear();
    groups_.clear();
    segments_.clear();
    stats_ = BatchStats{};
    built_ = false;
}

void SpriteBatch::reserve(usize quads) {
    keys_.reserve(quads);
    items_.reserve(quads);
    quads_.reserve(quads);
    vertices_.reserve(quads * 4);
}

u16 SpriteBatch::material(SpriteBlend blend, SpriteSetId set) {
    return static_cast<u16>((static_cast<u32>(blend) << 12) | (set.value & 0xFFFu));
}

void SpriteBatch::push_item(u64 key, ItemKind kind, u32 index) {
    keys_.push_back(key);
    items_.push_back(Item{kind, index});
}

void SpriteBatch::expand_sprite(const SpriteInstance& s, SpriteVertex out[4]) const {
    const SpriteRegion& r = sets_.region(s.set, s.region);
    const f32 w = r.width_m * s.scale.x;
    const f32 h = r.height_m * s.scale.y;
    const f32 x0 = -s.pivot.x * w, x1 = x0 + w;
    const f32 y0 = -s.pivot.y * h, y1 = y0 + h;
    f32 c = 1.0f, sn = 0.0f;
    if (s.rotation != 0.0f) {
        c = std::cos(s.rotation);
        sn = std::sin(s.rotation);
    }
    auto place = [&](f32 lx, f32 ly) {
        return vec2(s.position.x + lx * c - ly * sn, s.position.y + lx * sn + ly * c);
    };
    f32 ul = r.u0, ur = r.u1, vb = r.v1, vt = r.v0;
    if (s.flip_x) {
        const f32 t = ul;
        ul = ur;
        ur = t;
    }
    if (s.flip_y) {
        const f32 t = vb;
        vb = vt;
        vt = t;
    }
    const u32 color = pack_color_srgb(s.tint);
    const bool lit = sets_.lit(s.set);
    const u32 flags = (sets_.coverage(s.set) ? vertex_flags::kCoverage : 0u) | (sets_.sdf(s.set) ? vertex_flags::kSdf : 0u) |
                      (lit ? vertex_flags::kLit : 0u);
    const u32 packed = pack_layer_flags(r.layer, flags, lit ? pack_orientation(s.rotation, s.flip_x, s.flip_y) : 0u);
    const vec2 p0 = place(x0, y0), p1 = place(x1, y0), p2 = place(x1, y1), p3 = place(x0, y1);
    out[0] = {p0.x, p0.y, ul, vb, color, packed};
    out[1] = {p1.x, p1.y, ur, vb, color, packed};
    out[2] = {p2.x, p2.y, ur, vt, color, packed};
    out[3] = {p3.x, p3.y, ul, vt, color, packed};
}

bool SpriteBatch::visible(const SpriteVertex v[4]) const {
    aabb2 b{vec2(v[0].x, v[0].y), vec2(v[0].x, v[0].y)};
    for (u32 i = 1; i < 4; ++i) b = merge(b, aabb2{vec2(v[i].x, v[i].y), vec2(v[i].x, v[i].y)});
    return b.overlaps(view_.world_bounds);
}

void SpriteBatch::add(const SpriteInstance& s) {
    ++stats_.submitted;
    if (!s.visible || !sets_.valid(s.set)) {
        ++stats_.culled;
        return;
    }
    SpriteVertex v[4];
    expand_sprite(s, v);
    if (!visible(v)) {
        ++stats_.culled;
        return;
    }
    // Store the expanded quad: build() then only copies.
    RawQuad q;
    for (u32 i = 0; i < 4; ++i) q.v[i] = v[i];
    push_item(sort_key_2d(pass_, s.sort_layer, material(s.blend, s.set), s.sort_order), ItemKind::Quad,
              static_cast<u32>(quads_.size()));
    quads_.push_back(q);
}

void SpriteBatch::add_triangle(const SpriteTriangle& t) {
    ++stats_.submitted;
    if (!sets_.valid(t.set)) {
        ++stats_.culled;
        return;
    }
    const SpriteRegion& r = sets_.region(t.set, t.region);
    const bool lit = sets_.lit(t.set);
    const u32 flags = (sets_.coverage(t.set) ? vertex_flags::kCoverage : 0u) | (sets_.sdf(t.set) ? vertex_flags::kSdf : 0u) |
                      (lit ? vertex_flags::kLit : 0u);
    const u32 packed = pack_layer_flags(r.layer, flags, lit ? pack_orientation(t.rotation, t.flip_x, t.flip_y) : 0u);
    RawQuad q;
    for (u32 i = 0; i < 3; ++i) {
        const SpriteTriangle::Vertex& v = t.v[i];
        q.v[i] = {v.position.x, v.position.y, r.u0 + (r.u1 - r.u0) * v.uv.x, r.v0 + (r.v1 - r.v0) * v.uv.y,
                  pack_color_srgb(v.tint), packed};
    }
    q.v[3] = q.v[2];  // degenerate: the quad's second triangle (2, 3, 0) has no area
    if (!visible(q.v)) {
        ++stats_.culled;
        return;
    }
    ++stats_.triangles;
    push_item(sort_key_2d(pass_, t.sort_layer, material(t.blend, t.set), t.sort_order), ItemKind::Quad,
              static_cast<u32>(quads_.size()));
    quads_.push_back(q);
}

namespace {

// Cuts the span [a0, a1] (any order) to [lo, hi], moving its texture coordinates t0 / t1 with it.
// False when nothing is left.
bool clip_span(f32& a0, f32& a1, f32& t0, f32& t1, f32 lo, f32 hi) {
    const bool flip = a1 < a0;
    f32 x0 = flip ? a1 : a0, x1 = flip ? a0 : a1;
    f32 u0 = flip ? t1 : t0, u1 = flip ? t0 : t1;
    if (x1 <= lo || x0 >= hi) return false;
    const f32 len = x1 - x0;
    if (len > 0.0f) {
        const f32 c0 = x0 < lo ? lo : x0, c1 = x1 > hi ? hi : x1;
        const f32 du = u1 - u0;
        const f32 n0 = u0 + du * (c0 - x0) / len, n1 = u0 + du * (c1 - x0) / len;
        x0 = c0, x1 = c1, u0 = n0, u1 = n1;
    }
    a0 = flip ? x1 : x0, a1 = flip ? x0 : x1;
    t0 = flip ? u1 : u0, t1 = flip ? u0 : u1;
    return true;
}

}  // namespace

void SpriteBatch::add_rect(const RectQuad& q) {
    ++stats_.submitted;
    if (!sets_.valid(q.set)) {
        ++stats_.culled;
        return;
    }
    const SpriteRegion& r = sets_.region(q.set, q.region);
    f32 x0 = q.rect.min.x, x1 = q.rect.max.x, y0 = q.rect.min.y, y1 = q.rect.max.y;
    f32 u0 = r.u0 + (r.u1 - r.u0) * q.uv.min.x, u1 = r.u0 + (r.u1 - r.u0) * q.uv.max.x;
    f32 v0 = r.v0 + (r.v1 - r.v0) * q.uv.min.y, v1 = r.v0 + (r.v1 - r.v0) * q.uv.max.y;
    if (q.clip && (!clip_span(x0, x1, u0, u1, q.clip->min.x, q.clip->max.x) ||
                   !clip_span(y0, y1, v0, v1, q.clip->min.y, q.clip->max.y))) {
        ++stats_.culled;
        return;
    }
    const u32 color = pack_color_srgb(q.tint);
    const u32 packed = pack_layer_flags(r.layer, 0u);
    RawQuad raw;
    raw.v[0] = {x0, y1, u0, v1, color, packed};
    raw.v[1] = {x1, y1, u1, v1, color, packed};
    raw.v[2] = {x1, y0, u1, v0, color, packed};
    raw.v[3] = {x0, y0, u0, v0, color, packed};
    if (!visible(raw.v)) {
        ++stats_.culled;
        return;
    }
    push_item(sort_key_2d(pass_, q.sort_layer, material(q.blend, q.set), q.sort_order), ItemKind::Quad,
              static_cast<u32>(quads_.size()));
    quads_.push_back(raw);
}

void SpriteBatch::add_text(const Font& font, const TextLayout& layout, const TextPlacement& p) {
    if (!sets_.valid(font.set)) return;
    const u64 key = sort_key_2d(pass_, p.sort_layer, material(p.blend, font.set), p.sort_order);
    const u32 packed = pack_layer_flags(
        0, (sets_.coverage(font.set) ? vertex_flags::kCoverage : 0u) | (sets_.sdf(font.set) ? vertex_flags::kSdf : 0u));
    const f32 sy = p.y_down ? p.units_per_pixel : -p.units_per_pixel;
    // One copy of the glyphs, moved by (dx, dy) layout pixels (+y down).
    auto emit = [&](f32 dx, f32 dy, const math::Color& tint) {
        const u32 color = pack_color_srgb(tint);
        const f32 ox = p.origin.x + dx * p.units_per_pixel, oy = p.origin.y + dy * sy;
        for (const GlyphQuad& g : layout.quads) {
            ++stats_.submitted;
            f32 x0 = ox + g.x0 * p.units_per_pixel, x1 = ox + g.x1 * p.units_per_pixel;
            f32 yt = oy + g.y0 * sy, yb = oy + g.y1 * sy;
            f32 u0 = g.u0, u1 = g.u1, v0 = g.v0, v1 = g.v1;
            if (p.clip && (!clip_span(x0, x1, u0, u1, p.clip->min.x, p.clip->max.x) ||
                           !clip_span(yt, yb, v0, v1, p.clip->min.y, p.clip->max.y))) {
                ++stats_.culled;
                continue;
            }
            RawQuad q;
            q.v[0] = {x0, yb, u0, v1, color, packed};
            q.v[1] = {x1, yb, u1, v1, color, packed};
            q.v[2] = {x1, yt, u1, v0, color, packed};
            q.v[3] = {x0, yt, u0, v0, color, packed};
            if (!visible(q.v)) {
                ++stats_.culled;
                continue;
            }
            push_item(key, ItemKind::Quad, static_cast<u32>(quads_.size()));
            quads_.push_back(q);
        }
    };
    // Shadow, outline, fill: the radix sort is stable, so equal keys draw in this order.
    if ((p.shadow_px.x != 0.0f || p.shadow_px.y != 0.0f) && p.shadow_color.a > 0.0f)
        emit(p.shadow_px.x, p.shadow_px.y, p.shadow_color);
    if (p.outline_px > 0.0f && p.outline_color.a > 0.0f) {
        const f32 w = p.outline_px, d = 0.70710678f * p.outline_px;
        const f32 taps[8][2] = {{-w, 0}, {w, 0}, {0, -w}, {0, w}, {-d, -d}, {d, -d}, {-d, d}, {d, d}};
        for (const auto& t : taps) emit(t[0], t[1], p.outline_color);
    }
    emit(0.0f, 0.0f, p.tint);
}

void SpriteBatch::add_tilemap(u32 id, const TilemapBake& bake, vec2 offset) {
    if (!sets_.valid(bake.set) || bake.chunks.empty()) return;
    TilemapItem t{id, static_cast<u32>(tile_args_.size()), 0, 0, offset};
    for (const TilemapBake::Chunk& c : bake.chunks) {
        ++stats_.submitted;
        if (!aabb2{c.bounds.min + offset, c.bounds.max + offset}.overlaps(view_.world_bounds)) {
            ++stats_.culled;
            continue;
        }
        tile_args_.push_back({c.quad_count * 6, 1, 0, static_cast<i32>(c.first_quad * 4), 0});
        t.quads += c.quad_count;
    }
    t.arg_count = static_cast<u32>(tile_args_.size()) - t.first_arg;
    if (t.arg_count == 0) return;
    push_item(sort_key_2d(pass_, bake.sort_layer, material(bake.blend, bake.set), bake.sort_order), ItemKind::Tilemap,
              static_cast<u32>(tilemaps_.size()));
    tilemaps_.push_back(t);
}

void SpriteBatch::build() {
    MCE_ASSERT_MSG(!built_, "SpriteBatch::build twice (call begin first)");
    const usize n = items_.size();
    Array<u32> order(alloc_);
    order.resize(n);
    for (usize i = 0; i < n; ++i) order[i] = static_cast<u32>(i);
    radix_sort(keys_.span(), order.span(), alloc_);

    vertices_.reserve(quads_.size() * 4);
    bool group_open = false;
    bool segment_open = false;
    u16 group_material = 0;
    u32 group_first_quad = 0;  // relative to the open segment

    auto close_group = [&]() {
        if (!group_open) return;
        group_open = false;
        Group& g = groups_.back();
        const Segment& seg = segments_.back();
        const u32 seg_quads = seg.vertex_count / 4;
        u32 first = group_first_quad;
        u32 remaining = seg_quads - group_first_quad;
        g.first_command = static_cast<u32>(args_.size());
        while (remaining > 0) {
            const u32 q = math::min(remaining, kMaxQuadsPerDraw);
            args_.push_back({q * 6, 1, 0, static_cast<i32>(first * 4), 0});
            first += q;
            remaining -= q;
        }
        g.command_count = static_cast<u32>(args_.size()) - g.first_command;
    };

    for (usize i = 0; i < n; ++i) {
        const Item& it = items_[order[i]];
        const u16 mat = sort_key_material(keys_[i]);
        const auto blend = static_cast<SpriteBlend>(mat >> 12);
        const SpriteSetId set{static_cast<u16>(mat & 0xFFF)};
        if (it.kind == ItemKind::Tilemap) {
            close_group();
            segment_open = false;
            const TilemapItem& t = tilemaps_[it.index];
            Group g{GroupSource::Tilemap, blend, set, 0, t.id, static_cast<u32>(args_.size()), t.arg_count, t.offset};
            for (u32 a = 0; a < t.arg_count; ++a) args_.push_back(tile_args_[t.first_arg + a]);
            groups_.push_back(g);
            stats_.tile_quads += t.quads;
            continue;
        }
        if (!segment_open) {
            segments_.push_back(Segment{static_cast<u32>(vertices_.size()), 0});
            segment_open = true;
        }
        if (!group_open || mat != group_material) {
            close_group();
            groups_.push_back(Group{GroupSource::Dynamic, blend, set, static_cast<u32>(segments_.size() - 1), 0, 0, 0, {}});
            group_open = true;
            group_material = mat;
            group_first_quad = segments_.back().vertex_count / 4;
        }
        const RawQuad& q = quads_[it.index];
        for (const SpriteVertex& v : q.v) vertices_.push_back(v);
        segments_.back().vertex_count += 4;
    }
    close_group();

    stats_.quads = static_cast<u32>(vertices_.size() / 4);
    stats_.groups = static_cast<u32>(groups_.size());
    stats_.commands = static_cast<u32>(args_.size());
    built_ = true;
}

}  // namespace mce::render
