#pragma once
// The 2D batch: sprite sets, cook-baked tilemap layers and the per-frame sprite batch
// (rhi-renderer.md §3.2/§3.4, rendering-2d D1-D5, FR-R2D-010/020/060/115).
//
// Per frame: begin(view) -> add sprites / text / tilemaps (each culled against the view) ->
// build() sorts everything by the 64-bit key and expands quads into one SpriteVertex stream with
// draw groups. A group is a maximal run of items sharing (blend × sprite set) and source
// (dynamic stream or one tilemap layer); each group is ONE MultiDrawElementsIndirect. The
// builder is CPU-only and deterministic; Renderer::record_2d turns a built batch into commands.

#include "mce/containers/array.h"
#include "mce/render/camera2d.h"
#include "mce/render/sort_key.h"
#include "mce/render/text.h"
#include "mce/render/types.h"
#include "mce/result.h"

namespace mce::render {

// Largest quad run one indirect command covers: the shared quad index buffer holds this many
// quads (u32 indices, 4 vertices per quad, base_vertex per command). Longer runs split into
// several commands of the same MDI.
inline constexpr u32 kMaxQuadsPerDraw = 16384;

struct SpriteSetDesc {
    u32 texture_key = 0;                // TextureStore key (asset texture or renderer-local)
    Span<const SpriteRegion> regions;   // copied
    f32 pixels_per_meter = 48.0f;       // art density (rendering-2d §9)
    rhi::Filter filter = rhi::Filter::Nearest;  // pixel art default: point sampling, no mips
    bool coverage = false;              // font atlas: R8 coverage sampled as alpha
    const char* debug_name = "";
};

// CPU-side registry of sprite sets: region tables and material identity. The texture itself is
// resolved through the TextureStore at record time, so hot-reloaded art swaps in transparently.
class SpriteSets {
public:
    static constexpr u32 kMaxSets = 4096;  // 12 bits of the sort key's material field

    SpriteSetId create(const SpriteSetDesc& desc);
    // Replaces the region table / texture key of an existing set (atlas re-pack, hot reload).
    void update(SpriteSetId id, const SpriteSetDesc& desc);
    void destroy(SpriteSetId id);

    bool valid(SpriteSetId id) const;
    u32 texture_key(SpriteSetId id) const { return sets_[id.value].texture_key; }
    rhi::Filter filter(SpriteSetId id) const { return sets_[id.value].filter; }
    bool coverage(SpriteSetId id) const { return sets_[id.value].coverage; }
    f32 pixels_per_meter(SpriteSetId id) const { return sets_[id.value].pixels_per_meter; }
    u32 region_count(SpriteSetId id) const { return static_cast<u32>(sets_[id.value].regions.size()); }
    // Out-of-range regions resolve to the whole texture (layer 0), never to garbage.
    const SpriteRegion& region(SpriteSetId id, u32 index) const;

private:
    struct Set {
        u32 texture_key = 0;
        Array<SpriteRegion> regions;
        f32 pixels_per_meter = 48.0f;
        rhi::Filter filter = rhi::Filter::Nearest;
        bool coverage = false;
        bool alive = false;
    };
    Array<Set> sets_;
    Array<u16> free_;
    SpriteRegion fallback_;
};

// A font ready to draw: the layout face plus the coverage sprite set holding its atlas.
struct Font {
    FontFace face;
    SpriteSetId set;
};

// ---- tilemaps (rendering-2d D5, FR-R2D-115) -----------------------------------------------------

inline constexpr u32 kTileChunkSize = 32;  // tiles per chunk edge

struct TilemapDesc {
    SpriteSetId set;
    u32 width = 0, height = 0;       // tiles
    f32 tile_size = 1.0f;            // meters per tile edge
    math::vec2 origin;               // world position of the bottom-left corner of tile (0, 0)
    Span<const u32> tiles;           // width × height, row-major, row 0 = bottom; 0 = empty, n = region n-1
    math::Color tint = math::Color::white();
    i16 sort_layer = 0;
    i32 sort_order = 0;
    SpriteBlend blend = SpriteBlend::Alpha;
};

// The baked form of one tilemap layer: static quads grouped by 32×32 chunk (what the cook bakes
// into CHNK_TILE; the runtime bakes it the same way for editor edits). The renderer uploads
// `vertices` once into a persistent buffer; per frame only chunk visibility is computed.
struct TilemapBake {
    struct Chunk {
        math::aabb2 bounds;
        u32 first_quad = 0;
        u32 quad_count = 0;
    };
    Array<SpriteVertex> vertices;  // 4 per quad, chunk by chunk
    Array<Chunk> chunks;           // non-empty chunks only
    SpriteSetId set;
    i16 sort_layer = 0;
    i32 sort_order = 0;
    SpriteBlend blend = SpriteBlend::Alpha;
    u32 quad_count() const { return static_cast<u32>(vertices.size() / 4); }
};

ResultV bake_tilemap(const TilemapDesc& desc, const SpriteSets& sets, TilemapBake& out);

// ---- the per-frame batch ------------------------------------------------------------------------

// Where a text layout lands: pixel space is scaled by `units_per_pixel` and, unless y_down,
// flipped into the Y-up world. `origin` is the top-left of the layout box.
struct TextPlacement {
    math::vec2 origin;
    f32 units_per_pixel = 1.0f;  // 1 for the screen view; 1/ppm for world-space labels
    bool y_down = true;          // true for compute_screen_view, false for world views
    math::Color tint = math::Color::white();
    i16 sort_layer = 0;
    i32 sort_order = 0;
    SpriteBlend blend = SpriteBlend::Alpha;
};

struct BatchStats {
    u32 submitted = 0;  // sprites + glyphs + tilemap chunks offered
    u32 culled = 0;
    u32 quads = 0;      // dynamic quads written this frame
    u32 tile_quads = 0; // static tile quads drawn
    u32 groups = 0;     // = MDI submits
    u32 commands = 0;   // indirect commands across all groups
};

class SpriteBatch {
public:
    enum class GroupSource : u8 { Dynamic, Tilemap };

    struct Group {
        GroupSource source;
        SpriteBlend blend;
        SpriteSetId set;
        u32 segment;        // dynamic: which vertex segment (a run of dynamic groups between tilemaps)
        u32 tilemap;        // tilemap: caller's tilemap index (add_tilemap's `id`)
        u32 first_command;  // into draw_args()
        u32 command_count;
    };
    struct Segment {
        u32 first_vertex;
        u32 vertex_count;
    };

    // All per-frame storage comes from `frame` (the frame arena; mem::heap() in tests).
    SpriteBatch(const SpriteSets& sets, mem::IAllocator& frame);

    void begin(const View2D& view, PassId pass = PassId::World2D);
    void add(const SpriteInstance& sprite);
    void add_text(const Font& font, const TextLayout& layout, const TextPlacement& placement);
    // Culls the layer's chunks against the view and queues the visible ones as one item.
    void add_tilemap(u32 id, const TilemapBake& bake);
    void build();

    const View2D& view() const { return view_; }
    Span<const SpriteVertex> vertices() const { return vertices_.span(); }
    Span<const rhi::DrawIndexedIndirectCommand> draw_args() const { return args_.span(); }
    Span<const Group> groups() const { return groups_.span(); }
    Span<const Segment> segments() const { return segments_.span(); }
    const BatchStats& stats() const { return stats_; }
    bool built() const { return built_; }

private:
    enum class ItemKind : u8 { Quad, Tilemap };
    struct Item {
        ItemKind kind;
        u32 index;
    };
    struct RawQuad {  // a pre-expanded, already-culled quad (sprite or glyph)
        SpriteVertex v[4];
    };
    struct TilemapItem {
        u32 id;
        u32 first_arg;  // into tile_args_
        u32 arg_count;
        u32 quads;
    };

    void push_item(u64 key, ItemKind kind, u32 index);
    static u16 material(SpriteBlend blend, SpriteSetId set);
    void expand_sprite(const SpriteInstance& s, SpriteVertex out[4]) const;
    bool visible(const SpriteVertex v[4]) const;

    const SpriteSets& sets_;
    mem::IAllocator& alloc_;
    View2D view_;
    PassId pass_ = PassId::World2D;
    Array<u64> keys_;
    Array<Item> items_;
    Array<RawQuad> quads_;
    Array<TilemapItem> tilemaps_;
    Array<rhi::DrawIndexedIndirectCommand> tile_args_;
    // build() output
    Array<SpriteVertex> vertices_;
    Array<rhi::DrawIndexedIndirectCommand> args_;
    Array<Group> groups_;
    Array<Segment> segments_;
    BatchStats stats_;
    bool built_ = false;
};

}  // namespace mce::render
