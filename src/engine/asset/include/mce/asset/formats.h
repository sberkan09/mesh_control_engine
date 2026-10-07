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

// ------------------------------------------------------------------ scene (CHNK_SCENE)
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
