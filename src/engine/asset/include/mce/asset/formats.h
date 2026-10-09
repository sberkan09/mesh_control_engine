#pragma once
// Cooked chunk payload formats (assets.md §2) and their one reader/writer each. The cook writes
// these, the runtime reads them zero-copy out of the container; nothing here parses text.
//
// Every payload starts with a fixed header, all offsets are relative to the payload start and
// 16-byte aligned, and every reader bounds-checks before handing out views (payloads are
// untrusted bytes). No chunk embeds paths, timestamps or other cook-environment residue.

#include "mce/asset/container.h"

namespace mce::asset {

// Schema majors (TocEntry::chunk_version high byte) this build reads and writes.
inline constexpr u8 MANIFEST_VERSION = 1;
inline constexpr u8 TEXTURE_VERSION = 1;
inline constexpr u8 SFX_VERSION = 1;
inline constexpr u8 FONT_VERSION = 1;
inline constexpr u8 MATERIAL_VERSION = 1;
inline constexpr u8 SCENE_VERSION = 1;
inline constexpr u8 MESH_VERSION = 1;
inline constexpr u8 TILEMAP_VERSION = 1;  // CHNK_TILE: one tile layer (v0.5, FR-R2D-115)
inline constexpr u8 PARTICLE_VERSION = 1;  // CHNK_PART: one particle emitter preset (v0.5, FR-R2D-070)
inline constexpr u8 SEQUENCE_VERSION = 1;  // CHNK_SEQ: one action-list sequence (v0.5, FR-ENT-100)
inline constexpr u8 PREFAB_VERSION = 1;  // CHNK_Prefab: the scene layout below (v0.5, FR-ENT-070)

// ------------------------------------------------------------------ manifest (CHNK_Manifest)
// GUID -> {logical path, content hash, primary chunk type}. Entries sorted by GUID (the sort is
// part of the format); an entry's index is its AssetKey.

struct ManifestHeader {
    u32 entry_count;
    u32 strings_size;
    u32 reserved[2];
};
static_assert(sizeof(ManifestHeader) == 16);

struct ManifestEntry {
    u64 guid_hi;
    u64 guid_lo;
    u64 content_hash;  // xxh64 of the asset's cooked container: names cooked/<aa>/<guid>/<hash>.mce
    ChunkType type;    // primary chunk type (what AssetRef<T> must match)
    u16 reserved0;
    u32 path_offset;   // into the string blob: source path relative to assets/, forward slashes
    u32 path_size;
    u32 reserved1;

    Guid guid() const { return Guid{guid_hi, guid_lo}; }
};
static_assert(sizeof(ManifestEntry) == 40);

class ManifestView {
public:
    static Result<ManifestView> read(Span<const byte> payload);
    u32 size() const { return count_; }
    const ManifestEntry& entry(u32 i) const;
    StringView path(u32 i) const;
    // Binary search; size() when absent.
    u32 find(Guid g) const;

private:
    const ManifestEntry* entries_ = nullptr;
    const char* strings_ = nullptr;
    u32 count_ = 0;
};

struct ManifestItem {
    Guid guid;
    u64 content_hash = 0;
    ChunkType type = ChunkType::Manifest;
    StringView path;
};
// Sorts by GUID and serializes. Duplicate GUIDs are a programmer error (the registry rejects them).
void write_manifest(Span<ManifestItem> items, Array<byte>& out);

// ------------------------------------------------------------------ texture (CHNK_TEX)

enum class TextureFormat : u32 {
    RGBA8 = 1,  // MVP; BC1/BC5/BC7 at v0.5
};

namespace TextureFlags {
enum : u32 {
    Srgb = 1u << 0,  // color data is sRGB-encoded; the renderer samples through an sRGB view
};
}

struct TextureHeader {
    TextureFormat format;
    u32 width;
    u32 height;
    u32 layers;
    u32 mip_count;
    u32 flags;  // TextureFlags
    u32 reserved[2];
};
static_assert(sizeof(TextureHeader) == 32);

struct TextureMip {
    u32 width;
    u32 height;
    u64 offset;  // from payload start, layer 0; layers follow contiguously (layer_size each)
    u64 layer_size;
};
static_assert(sizeof(TextureMip) == 24);

struct TextureView {
    const TextureHeader* header = nullptr;
    Span<const TextureMip> mips;
    Span<const byte> payload;

    Span<const byte> pixels(u32 mip, u32 layer = 0) const;
    static Result<TextureView> read(Span<const byte> payload);
};

// One layer's mip chain, mip 0 first, tightly packed RGBA8 rows.
struct TextureMipData {
    u32 width;
    u32 height;
    Span<const byte> pixels;  // width * height * 4 * layers bytes (layers contiguous)
};
void write_texture(TextureFormat format, u32 layers, u32 flags, Span<const TextureMipData> mips, Array<byte>& out);

// ------------------------------------------------------------------ audio clip (CHNK_SFX)

namespace SfxFlags {
enum : u32 {
    Looping = 1u << 0,
};
}

struct SfxHeader {
    u32 sample_rate;   // 48000 after cook
    u32 channels;      // 1 or 2, interleaved
    u64 frame_count;
    u64 loop_start;    // frames; loop_end > loop_start when Looping
    u64 loop_end;
    u32 flags;         // SfxFlags
    f32 gain;          // normalization gain the cook applied (informational)
    u64 data_offset;   // float32 interleaved samples
};
static_assert(sizeof(SfxHeader) == 48);

struct SfxView {
    const SfxHeader* header = nullptr;
    Span<const f32> samples;  // frame_count * channels
    static Result<SfxView> read(Span<const byte> payload);
};
void write_sfx(const SfxHeader& header, Span<const f32> samples, Array<byte>& out);

// ------------------------------------------------------------------ font (CHNK_FONT)

struct FontHeader {
    f32 pixel_height;
    f32 ascent;    // pixels at pixel_height, positive up
    f32 descent;   // pixels, negative
    f32 line_gap;
    u32 atlas_width;
    u32 atlas_height;
    u32 glyph_count;
    u32 kern_count;
    u64 glyph_offset;
    u64 kern_offset;
    u64 atlas_offset;  // R8 coverage, atlas_width * atlas_height
    u64 reserved;
};
static_assert(sizeof(FontHeader) == 64);

struct FontGlyph {  // sorted by codepoint
    u32 codepoint;
    u16 x0, y0, x1, y1;  // atlas rect
    f32 xoff, yoff;      // quad offset from the pen position (y down)
    f32 xoff2, yoff2;
    f32 xadvance;
};
static_assert(sizeof(FontGlyph) == 32);

struct FontKern {  // sorted by (left, right)
    u32 left;
    u32 right;
    f32 advance;  // pixels to add between the pair
};
static_assert(sizeof(FontKern) == 12);

struct FontView {
    const FontHeader* header = nullptr;
    Span<const FontGlyph> glyphs;
    Span<const FontKern> kerning;
    Span<const byte> atlas;

    const FontGlyph* find(u32 codepoint) const;
    f32 kern(u32 left, u32 right) const;
    static Result<FontView> read(Span<const byte> payload);
};
void write_font(const FontHeader& header, Span<const FontGlyph> glyphs, Span<const FontKern> kerning,
                Span<const byte> atlas, Array<byte>& out);

// ------------------------------------------------------------------ material (CHNK_MAT)

enum class MaterialSlot : u32 { BaseColor = 0, Normal, MetallicRoughness, Emissive, Count };

namespace MaterialFlags {
enum : u32 {
    AlphaBlend = 1u << 0,
    AlphaMask = 1u << 1,
    DoubleSided = 1u << 2,
    Unlit = 1u << 3,
    // M5a (ADR-0024): the MetallicRoughness texture is an ORM map whose R channel is ambient
    // occlusion (glTF occlusionTexture packed with metallicRoughnessTexture).
    OcclusionInMR = 1u << 4,
    // The MetallicRoughness slot holds an occlusion-only map: R is AO, G/B are not data.
    OcclusionOnly = 1u << 5,
};
}

struct MaterialChunk {  // the whole payload: fixed-width param block + texture refs by GUID
    f32 base_color[4];
    f32 emissive[3];
    f32 metallic;
    f32 roughness;
    f32 alpha_cutoff;
    u32 flags;  // MaterialFlags
    u32 reserved;
    u64 textures[static_cast<u32>(MaterialSlot::Count)][2];  // {hi, lo}; null = no texture

    Guid texture(MaterialSlot s) const { return Guid{textures[static_cast<u32>(s)][0], textures[static_cast<u32>(s)][1]}; }
    static Result<const MaterialChunk*> read(Span<const byte> payload);
};
static_assert(sizeof(MaterialChunk) == 112);

// ------------------------------------------------------------------ tilemap (CHNK_TILE)
// One tile layer (FR-R2D-115, ADR-0030 §8): a width x height grid of tile indices, row 0 at the
// bottom, over a tileset texture cut into `columns` x `rows` equal cells. Tile 0 is empty; tile n
// is cell n-1, counted left to right from the top row of the texture. The runtime bakes it into
// static quads (render::bake_tilemap) placed at the owning entity.
struct TilemapHeader {
    u64 tileset[2];  // texture GUID {hi, lo}
    u32 width;       // tiles
    u32 height;
    u32 columns;     // tileset cells across
    u32 rows;        // tileset cells down
    f32 tile_size;   // metres per tile edge
    u32 reserved[3];
    // u32 tiles[width * height] follow
    Guid tileset_guid() const { return Guid{tileset[0], tileset[1]}; }
};
static_assert(sizeof(TilemapHeader) == 48);

struct TilemapView {
    const TilemapHeader* header = nullptr;
    Span<const u32> tiles;
    static Result<TilemapView> read(Span<const byte> payload);
};

// ------------------------------------------------------------------ particle preset (CHNK_PART)
// One emitter's parameters (FR-R2D-070, assets.md §2): the kit's particle system simulates them on
// the CPU and draws every particle through the sprite batch. Ranges are {min, max}, picked per
// particle from the emitter's seeded generator; colors are linear RGBA.
namespace ParticleFlags {
inline constexpr u32 Additive = 1u << 0;  // additive blend (glows, sparks) instead of alpha "over"
}

struct ParticlePresetChunk {  // the whole payload
    u64 texture[2];           // sprite texture GUID {hi, lo}; null = a solid quad in the color
    f32 rate;                 // particles per second while emitting (0 = bursts only)
    u32 burst;                // particles emitted at once when the emitter starts
    u32 max_particles;        // live cap; emission past it is dropped
    f32 duration;             // seconds of emission; 0 = emit until stopped
    f32 lifetime[2];          // seconds
    f32 speed[2];             // metres per second
    f32 direction;            // radians, 0 = +X, CCW
    f32 spread;               // half-angle around `direction`, radians
    f32 gravity[2];           // metres per second squared
    f32 drag;                 // velocity damping per second
    f32 size_start;           // metres (square)
    f32 size_end;
    f32 spin[2];              // radians per second
    f32 area[2];              // half extents of the emission box (0, 0 = a point)
    f32 color_start[4];
    f32 color_end[4];
    u32 flags;                // ParticleFlags
    u32 reserved[4];
    Guid texture_guid() const { return Guid{texture[0], texture[1]}; }
    static Result<const ParticlePresetChunk*> read(Span<const byte> payload);
};
static_assert(sizeof(ParticlePresetChunk) == 144);

// ------------------------------------------------------------------ sequence (CHNK_SEQ)
// One action-list sequence (FR-ENT-100, scripting.md §2.6): a name (the id SequencePlayer
// components play) and a closed list of steps the kit's SequenceRunner advances.
enum class SequenceStepKind : u8 {
    Wait, WaitForEvent, MoveTo, PlayAnim, PlaySound, StopSound, Emit, Call, Spawn, Destroy, End,
};

namespace SequenceStepFlags {
inline constexpr u8 Relative = 1u << 0;  // MoveTo / Spawn: `pos` is relative to the entity
}

struct SequenceStep {
    SequenceStepKind kind;
    u8 flags;            // SequenceStepFlags
    u8 pad_[6];
    u64 id;              // StringId: event / clip / call name
    u64 guid[2];         // PlaySound sfx, Spawn prefab {hi, lo}
    f32 a;               // Wait seconds; MoveTo speed; PlaySound gain dB
    f32 b;               // MoveTo arrival tolerance
    f32 pos[3];          // MoveTo target / Spawn position
    u32 pad2_[3];
    Guid asset() const { return Guid{guid[0], guid[1]}; }
};
static_assert(sizeof(SequenceStep) == 64);

struct SequenceHeader {
    char name[32];       // NUL-padded; the id is StringId(name)
    u32 step_count;
    u32 flags;           // bit 0: loops
    u32 reserved[2];
    // SequenceStep steps[step_count] follow (the last is End)
};
static_assert(sizeof(SequenceHeader) == 48);

struct SequenceView {
    const SequenceHeader* header = nullptr;
    Span<const SequenceStep> steps;
    StringView name() const;
    bool loops() const { return (header->flags & 1u) != 0; }
    static Result<SequenceView> read(Span<const byte> payload);
};

// ------------------------------------------------------------------ mesh (CHNK_MESH)
// A static glTF scene (assets.md §5.2, FR-R3D-010): meshopt-optimized, quantized vertex streams,
// one submesh per glTF primitive, the node tree that places them, and the file's node-transform
// animation clips. Geometry is node-local; the renderer composes node world matrices (rest pose,
// or a sampled clip) with the instance transform.
//
// Quantization (assets.md §5.2): positions are snorm16 relative to the submesh's bounds
// (p = center + q * extent), normals are octahedral snorm16, UVs are unorm16 over the submesh's
// UV rectangle (uv = uv_offset + q * uv_scale). Indices are relative to the submesh's
// base_vertex, u16 unless MeshFlags::Index32.
//
// Deviation (recorded in ADR-0022): animation clips ride in this chunk instead of a separate
// CHNK_ANIM — at MVP a clip only ever animates the nodes of the file it came from.

namespace MeshFlags {
enum : u32 {
    Index32 = 1u << 0,
};
}

struct MeshVertex {
    i16 position[4];  // snorm16; [3] is zero padding
    i16 normal[2];    // octahedral, snorm16
    u16 uv[2];        // unorm16
};
static_assert(sizeof(MeshVertex) == 16);

struct MeshHeader {
    u32 vertex_count;
    u32 index_count;
    u32 submesh_count;
    u32 node_count;
    u32 clip_count;
    u32 channel_count;
    u32 key_count;     // f32 values in the key pool
    u32 flags;         // MeshFlags
    f32 bounds_min[3]; // rest pose, model space
    f32 bounds_max[3];
    u32 strings_size;
    u32 reserved;
    u64 vertices_offset;
    u64 indices_offset;
    u64 submeshes_offset;
    u64 nodes_offset;
    u64 clips_offset;
    u64 channels_offset;
    u64 keys_offset;
    u64 strings_offset;
};
static_assert(sizeof(MeshHeader) == 128);

struct MeshSubmesh {
    u32 first_index;
    u32 index_count;
    u32 base_vertex;
    u32 vertex_count;
    u32 node;          // the node whose world transform places it
    u32 reserved[3];
    f32 center[3];     // position dequantization; node-local bounds are center +- extent
    f32 extent[3];
    f32 uv_offset[2];
    f32 uv_scale[2];
    MaterialChunk material;  // inline glTF material (texture refs by GUID)
    u32 reserved2[2];
};
static_assert(sizeof(MeshSubmesh) == 192);

struct MeshNode {  // parents precede children
    i32 parent;    // -1 = root
    u32 name_offset;
    u32 name_size;
    u32 reserved;
    f32 translation[3];
    f32 scale[3];
    f32 rotation[4];  // quaternion x, y, z, w
};
static_assert(sizeof(MeshNode) == 56);

struct MeshClip {
    u32 name_offset;
    u32 name_size;
    f32 duration;      // seconds: the last key time over its channels
    u32 first_channel;
    u32 channel_count;
    u32 reserved[3];
};
static_assert(sizeof(MeshClip) == 32);

enum class AnimPath : u32 { Translation = 0, Rotation = 1, Scale = 2 };
// CubicSpline (M5a): glTF CUBICSPLINE, values stored as {in-tangent, value, out-tangent} per key.
enum class AnimInterp : u32 { Linear = 0, Step = 1, CubicSpline = 2 };

struct MeshChannel {
    u32 node;
    AnimPath path;
    AnimInterp interp;
    u32 key_count;
    u32 times_offset;   // index into the key pool: key_count ascending times
    u32 values_offset;  // index into the key pool: key_count * (3 | 4) values (x3 for CubicSpline)
    u32 reserved[2];
};
static_assert(sizeof(MeshChannel) == 32);

struct MeshView {
    const MeshHeader* header = nullptr;
    Span<const MeshVertex> vertices;
    Span<const byte> indices;  // u16 or u32 per MeshFlags::Index32
    Span<const MeshSubmesh> submeshes;
    Span<const MeshNode> nodes;
    Span<const MeshClip> clips;
    Span<const MeshChannel> channels;
    Span<const f32> keys;
    Span<const char> strings;

    bool index32() const { return header && (header->flags & MeshFlags::Index32); }
    u32 index(u32 i) const;
    StringView node_name(u32 node) const;
    StringView clip_name(u32 clip) const;
    // Clip index by name, or clips.size() when absent.
    u32 find_clip(StringView name) const;
    // Validates every offset, every index against its submesh's vertex range, node parents and
    // channel ranges: the payload is untrusted bytes and its indices reach the GPU.
    static Result<MeshView> read(Span<const byte> payload);
};

struct MeshData {
    Span<const MeshVertex> vertices;
    Span<const byte> indices;  // u16 or u32 per `flags`
    Span<const MeshSubmesh> submeshes;
    Span<const MeshNode> nodes;
    Span<const MeshClip> clips;
    Span<const MeshChannel> channels;
    Span<const f32> keys;
    Span<const char> strings;
    u32 flags = 0;
    f32 bounds_min[3] = {0, 0, 0};
    f32 bounds_max[3] = {0, 0, 0};
};
void write_mesh(const MeshData& data, Array<byte>& out);

// ------------------------------------------------------------------ scene (CHNK_SCENE) / prefab (CHNK_PREFAB)
// CHNK_PREFAB payloads use this exact layout too (one reader, SceneView): a prefab is a cooked
// entity sub-tree whose entity 0 is the root, with nested prefabs already flattened by the cook.
// The columnar transform of a .mcscene (assets.md §6, FR-ENT-060). Per component type one
// column: the entity indices that carry it (ascending) and the raw component rows, laid out
// exactly as the reflected C++ type. AssetRef fields hold the GUID with a null handle; EntityRef
// fields hold the scene-local entity index in their first u32 (0xFFFFFFFF = none). Both are
// listed in fixup tables so the loader stamps handles / runtime ids without reflection.

struct SceneHeader {
    u32 format_version;  // the .mcscene "version" the cook accepted
    u32 entity_count;
    u32 column_count;
    u32 asset_ref_count;
    u32 asset_fixup_count;
    u32 entity_fixup_count;
    u64 source_hash;     // xxh64 of the canonical serialization of the source document
    u64 columns_offset;
    u64 asset_refs_offset;
    u64 asset_fixups_offset;
    u64 entity_fixups_offset;
    u64 names_offset;    // entity_count x {u32 offset, u32 size} then the string blob
    u64 names_size;
};
static_assert(sizeof(SceneHeader) == 80);

struct SceneColumn {  // sorted by type name
    u64 type_id;      // StringId of the reflected type name
    u32 type_size;
    u32 type_align;
    u32 row_count;
    u32 reserved;
    u64 rows_offset;  // u32 entity index per row
    u64 data_offset;  // row_count * type_size bytes
};
static_assert(sizeof(SceneColumn) == 40);

struct SceneAssetRef {  // unique, sorted by GUID
    u64 guid_hi;
    u64 guid_lo;
    u64 type_id;  // StringId of the asset type name ("Texture", ...)

    Guid guid() const { return Guid{guid_hi, guid_lo}; }
};
static_assert(sizeof(SceneAssetRef) == 24);

struct SceneFixup {
    u32 column;
    u32 row;
    u32 byte_offset;  // within the row
    u32 index;        // asset fixups: index into the asset ref table; entity fixups: unused (0)
};
static_assert(sizeof(SceneFixup) == 16);

struct SceneView {
    const SceneHeader* header = nullptr;
    Span<const SceneColumn> columns;
    Span<const SceneAssetRef> asset_refs;
    Span<const SceneFixup> asset_fixups;
    Span<const SceneFixup> entity_fixups;
    Span<const byte> payload;

    Span<const u32> rows(u32 column) const;
    Span<const byte> data(u32 column) const;
    const SceneColumn* find_column(u64 type_id) const;
    StringView entity_name(u32 entity) const;
    static Result<SceneView> read(Span<const byte> payload);
};

}  // namespace mce::asset
