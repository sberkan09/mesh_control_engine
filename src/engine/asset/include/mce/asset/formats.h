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
inline constexpr u8 SFX_VERSION = 1;  // minor 1: SfxFlags::Vorbis payloads (M-AUDIO2 1); minor 2: sound sets (M-AUDIO2 2)
inline constexpr u8 FONT_VERSION = 1;
inline constexpr u8 MATERIAL_VERSION = 1;
inline constexpr u8 MATERIAL_MINOR_EXT = 1;  // minor of a material chunk followed by a MaterialExt
inline constexpr u8 SCENE_VERSION = 1;
inline constexpr u8 MESH_VERSION = 1;
inline constexpr u8 MESH_MINOR_SKIN = 1;  // minor of a chunk with MeshFlags::Skinned
inline constexpr u8 MESH_MINOR_LODS = 2;  // minor of a chunk with MeshFlags::Lods (skinned or not)
inline constexpr u8 MESH_MINOR_RAGDOLL = 3;  // minor of a chunk with MeshFlags::Ragdoll
inline constexpr u8 MESH_MINOR_MATERIALS = 4;  // minor of a chunk with MeshFlags::Materials
inline constexpr u8 TILEMAP_VERSION = 1;  // CHNK_TILE: one tile layer (v0.5, FR-R2D-115)
inline constexpr u8 PARTICLE_VERSION = 1;  // CHNK_PART: one particle emitter preset (v0.5, FR-R2D-070)
inline constexpr u8 SEQUENCE_VERSION = 1;  // CHNK_SEQ: one action-list sequence (v0.5, FR-ENT-100)
inline constexpr u8 TIMELINE_VERSION = 1;  // CHNK_TIMELINE: one keyed timeline (M-CINEMA 1, FR-ENT-110)
inline constexpr u8 SHADER_VERSION = 1;  // CHNK_SHD: one validated GLSL stage + variants + reflection (v0.5)
inline constexpr u8 PREFAB_VERSION = 1;  // CHNK_Prefab: the scene layout below (v0.5, FR-ENT-070)
inline constexpr u8 MOD_INFO_VERSION = 1;  // CHNK_MOD: a mod pack's identity (FR-RUN-090)
inline constexpr u8 INPUT_MAP_VERSION = 1;  // CHNK_INPUT: a project's action maps (FR-INP-020; mce/input/input_map.h)
inline constexpr u8 STRING_TABLE_VERSION = 1;  // CHNK_STR: one locale's string table (M-MOBILE 11a; mce/loc/string_table.h)
inline constexpr u8 STORE_CATALOG_VERSION = 1;  // CHNK_STORE: products and ad placements (M-MONETIZE; mce/monetize/catalog.h)
inline constexpr u8 GAME_SERVICES_CATALOG_VERSION = 2;  // CHNK_GSVC: game services catalog (M-LIVEOPS; v2 Steam ids, M-STEAM 2; mce/liveops/game_services.h)
inline constexpr u8 ANIMGRAPH_VERSION = 1;  // CHNK_ANIMG: a 3D animation state graph (M-DATA-REACH 7)
inline constexpr u8 SPRITESHEET_VERSION = 1;  // CHNK_SHEET: texture regions + flipbook clips (M-DATA-REACH 5)
inline constexpr u8 UI_DOCUMENT_VERSION = 3;  // CHNK_UIDOC: one UI widget tree (M-UI-DATA 1; v2 actions + bindings; v3 theme + font roles; mce/ui/document_data.h)
inline constexpr u8 UI_THEME_VERSION = 1;     // CHNK_UITHEME: a UI theme (M-UI-DATA 3; mce/ui/document_data.h)
inline constexpr u8 TERRAIN_VERSION = 1;      // CHNK_TERRAIN: a heightmap terrain (M-TERRAIN 1)
inline constexpr u8 NAVMESH_VERSION = 1;     // CHNK_NAV: the .mcnav bytes as baked (M-GAMEPLAY3D 1; mce/physics3d/navmesh3d.h)
inline constexpr u8 VIDEO_VERSION = 1;       // CHNK_VID: an MPEG-1 program stream + its audio as PCM (M-CINEMA 5)
inline constexpr u8 BEHAVIOR_TREE_VERSION = 2;  // CHNK_BT: one behavior tree (M-AI 1; mce/kit/ai.h); 2: guard aborts, task arguments (M-AI 2)
inline constexpr u8 SKELETON2D_VERSION = 1;  // CHNK_SKEL2D: a 2D cut-out skeleton, bones + slots (M-ANIM2D 1)
inline constexpr u8 REMOTE_CONFIG_VERSION = 1;  // CHNK_RCFG: remote config defaults (M-LIVEOPS; mce/liveops/remote_config.h)

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
    RGBA8 = 1,     // MVP; BC1/BC5/BC7 at v0.5
    ASTC_4x4 = 2,  // phones (M-MOBILE step 8, mce_cook --textures astc): 16-byte blocks of 4x4 texels
};

// Bytes of one layer of a w x h level in `format` (0 for an unknown format).
u64 texture_level_bytes(TextureFormat format, u32 width, u32 height);

namespace TextureFlags {
enum : u32 {
    Srgb = 1u << 0,  // color data is sRGB-encoded; the renderer samples through an sRGB view
    LinearFilter = 1u << 1,  // kit sprites sample it bilinearly (import setting "filter": "linear"); else point
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

// One layer's mip chain, mip 0 first: tightly packed RGBA8 rows, or ASTC blocks row by row.
struct TextureMipData {
    u32 width;
    u32 height;
    Span<const byte> pixels;  // texture_level_bytes(format, width, height) * layers bytes (layers contiguous)
};
void write_texture(TextureFormat format, u32 layers, u32 flags, Span<const TextureMipData> mips, Array<byte>& out);

// ------------------------------------------------------------------ audio clip (CHNK_SFX)

namespace SfxFlags {
enum : u32 {
    Looping = 1u << 0,
    Vorbis = 1u << 1,  // data_offset holds a u64 byte count and an Ogg Vorbis stream (M-AUDIO2 1)
    Set = 1u << 2,     // a sound set (.mcsound, M-AUDIO2 2): data_offset holds a SoundSetHeader and its clips
};
}

// A sound set: clips picked per play, with a pitch and gain range. It cooks into an Sfx chunk so
// everything that names an Sfx (AudioSource, SfxTrigger, PlaySfx, timeline sound keys) can name a
// set; audio::ClipBank does the picking. The header's frame_count is 0.
enum class SoundSetPick : u32 { Random = 0, Sequence = 1 };
namespace SoundSetFlags {
enum : u32 {
    NoRepeat = 1u << 0,  // Random never picks the clip it picked last (with two or more clips)
};
}
struct SoundSetHeader {
    u32 clip_count;  // 0..4096 (an empty set plays nothing)
    SoundSetPick pick;
    f32 pitch_min, pitch_max;      // playback rate, 1 = as recorded
    f32 gain_min_db, gain_max_db;  // added to the play's own gain
    u32 flags;                     // SoundSetFlags
    u32 reserved;
    // followed by clip_count Guids (the member Sfx assets)
};
static_assert(sizeof(SoundSetHeader) == 32);

struct SfxHeader {
    u32 sample_rate;   // 48000 after cook
    u32 channels;      // 1 or 2, interleaved
    u64 frame_count;
    u64 loop_start;    // frames; loop_end > loop_start when Looping
    u64 loop_end;
    u32 flags;         // SfxFlags
    f32 gain;          // normalization gain the cook applied (informational)
    u64 data_offset;   // float32 interleaved samples, or (Vorbis) a u64 byte count then the stream
};
static_assert(sizeof(SfxHeader) == 48);

struct SfxView {
    const SfxHeader* header = nullptr;
    Span<const f32> samples;  // frame_count * channels (empty for Vorbis)
    Span<const byte> vorbis;  // the Ogg Vorbis stream (Vorbis only); it decodes to frame_count frames
    const SoundSetHeader* set = nullptr;  // a sound set (SfxFlags::Set) and its member clips
    Span<const Guid> set_clips;
    static Result<SfxView> read(Span<const byte> payload);
};
void write_sfx(const SfxHeader& header, Span<const f32> samples, Array<byte>& out);
// A Vorbis clip: `header` without SfxFlags::Vorbis is fine, the writer sets it.
void write_sfx_vorbis(const SfxHeader& header, Span<const byte> stream, Array<byte>& out);
// A sound set (an Sfx chunk with SfxFlags::Set and frame_count 0).
void write_sound_set(const SoundSetHeader& set, Span<const Guid> clips, Array<byte>& out);

// ------------------------------------------------------------------ video (CHNK_VID)
// The .mpg's bytes as imported (an MPEG-1 program stream; the runtime decodes its video with
// render::VideoDecoder) and its audio decoded once at cook time to 48 kHz f32, so the mixer plays
// it like any clip (ADR-0043). VideoHeader | mpeg bytes | f32 samples (audio_frames * audio_channels).

struct VideoHeader {
    u32 width;           // display size in pixels
    u32 height;
    f32 frame_rate;      // frames per second
    f32 duration;        // seconds of video
    u32 flags;           // 0 (reserved)
    u32 audio_channels;  // 0 = silent, else 2 (interleaved)
    u32 audio_frames;    // 48 kHz frames
    f32 audio_gain;      // linear gain the cook applied (informational)
    u64 mpeg_offset;
    u64 mpeg_size;
    u64 audio_offset;    // f32 interleaved samples
    u32 reserved[2];
};
static_assert(sizeof(VideoHeader) == 64);

struct VideoView {
    const VideoHeader* header = nullptr;
    Span<const byte> mpeg;
    Span<const f32> audio;  // audio_frames * audio_channels
    static Result<VideoView> read(Span<const byte> payload);
};
void write_video(const VideoHeader& header, Span<const byte> mpeg, Span<const f32> audio, Array<byte>& out);

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
    u64 atlas_offset;  // R8, atlas_width * atlas_height: coverage, or a distance field (FontFlags::Sdf)
    u32 flags;         // FontFlags
    f32 sdf_spread;    // Sdf: pixels at pixel_height from the edge (128) to 0 or 255
};
static_assert(sizeof(FontHeader) == 64);

namespace FontFlags {
inline constexpr u32 Sdf = 1u << 0;  // single-channel signed distance field: 128 is the glyph edge
}

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
    u32 uv_scale_half;  // texture repeats across the mesh's UVs (.mcmat uv_scale): two IEEE halves, u low, v high; 0 = [1, 1]
    u64 textures[static_cast<u32>(MaterialSlot::Count)][2];  // {hi, lo}; null = no texture

    Guid texture(MaterialSlot s) const { return Guid{textures[static_cast<u32>(s)][0], textures[static_cast<u32>(s)][1]}; }
    void uv_scale(f32 out[2]) const;
    static u32 pack_uv_scale(f32 u, f32 v);
    static Result<const MaterialChunk*> read(Span<const byte> payload);
};
static_assert(sizeof(MaterialChunk) == 112);

// M-VISUAL2 4: the lobes glTF's KHR_materials_clearcoat, _sheen, _anisotropy, _transmission and _ior
// add, factors only (their textures are not cooked). A .mcmat chunk carries one after its
// MaterialChunk (chunk minor MATERIAL_MINOR_EXT); a mesh, one per submesh (MeshFlags::Materials).
// All zero is the plain material: nothing changes in how it draws.
struct MaterialExt {
    f32 clearcoat;            // 0..1: a clear varnish layer over the base
    f32 clearcoat_roughness;  // the varnish's own roughness
    f32 sheen_color[3];       // linear; 0 = no sheen (cloth's soft glow at grazing angles)
    f32 sheen_roughness;
    f32 anisotropy;           // -1..1: highlights stretched along the UV tangent (> 0) or bitangent (< 0)
    f32 anisotropy_rotation;  // radians around the normal, from the UV tangent
    f32 transmission;         // 0..1: light passes through, bending the opaque frame behind
    f32 ior;                  // index of refraction; 0 = 1.5 (the dielectric F0 of 0.04)
    u32 reserved[6];

    bool any() const {
        return clearcoat > 0.0f || sheen_color[0] > 0.0f || sheen_color[1] > 0.0f || sheen_color[2] > 0.0f || anisotropy != 0.0f ||
               transmission > 0.0f || ior > 0.0f;
    }
    // The extension after a material chunk's MaterialChunk, or all zero without one.
    static MaterialExt of_material(Span<const byte> payload);
};
static_assert(sizeof(MaterialExt) == 64);

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

// ------------------------------------------------------------------ sprite sheet (CHNK_SHEET)
// Regions of one texture and named flipbook clips over them (M-DATA-REACH 5; played by the kit's
// AnimatedSprite through render/sprite_animation.h). Layout: the header, then region_count
// regions, clip_count clips, frame_count u32 frames (region indices; each clip owns a run of
// them) and event_count events (each clip owns a run; frame is within the clip).
struct SpriteSheetHeader {
    u64 texture[2];  // texture GUID {hi, lo}; zero = none
    u32 region_count;
    u32 clip_count;
    u32 frame_count;
    u32 event_count;
    u32 reserved[4];
    Guid texture_guid() const { return Guid{texture[0], texture[1]}; }
};
static_assert(sizeof(SpriteSheetHeader) == 48);

struct SpriteSheetRegion {  // normalized texture coordinates, v0 = top edge
    f32 u0, v0, u1, v1;
};
static_assert(sizeof(SpriteSheetRegion) == 16);

struct SpriteSheetClip {
    char name[32];  // NUL-padded
    u32 first_frame;
    u32 frame_count;
    u32 first_event;
    u32 event_count;
    f32 fps;
    u8 mode;  // render::PlayMode: 0 Loop, 1 PingPong, 2 Once
    u8 reserved[3];
};
static_assert(sizeof(SpriteSheetClip) == 56);

struct SpriteSheetEvent {
    u16 frame;     // frame index within its clip
    u16 reserved;
    u32 id;        // low 32 bits of fnv1a64(name): render::AnimEvent::id
    char name[24]; // NUL-padded
};
static_assert(sizeof(SpriteSheetEvent) == 32);

struct SpriteSheetView {
    const SpriteSheetHeader* header = nullptr;
    Span<const SpriteSheetRegion> regions;
    Span<const SpriteSheetClip> clips;
    Span<const u32> frames;
    Span<const SpriteSheetEvent> events;
    static Result<SpriteSheetView> read(Span<const byte> payload);  // checks every index
    // The clip named `name` (the first clip for an empty name), or null.
    const SpriteSheetClip* find_clip(StringView name) const;
    static StringView name_of(const SpriteSheetClip& c);
    static StringView name_of(const SpriteSheetEvent& e);
};

// ------------------------------------------------------------------ animation graph (CHNK_ANIMG)
// A 3D animation state graph (M-DATA-REACH 7) the kit's Animator runs over its MeshRef's clips:
// states (one clip, or a 1D or 2D blend of clips along parameters), transitions on parameter
// conditions or a one-shot's end, and timed events per state. Layout: the header, then
// state_count states, transition_count transitions, condition_count conditions,
// blend_point_count blend points and event_count events; every index is checked by read().
//
// M-GAMEPLAY3D step 5 extends it (header.flags & kAnimGraphExtended, chunk minor 1): after the
// events, an AnimGraphExtension, then its 2D blend points, layers, mask bones and IK chains. A
// graph that uses none of them is written without the extension, byte for byte as before.
inline constexpr u32 kAnimGraphMaxParams = 4;
inline constexpr u32 kAnimAnyState = 0xFFFFFFFFu;
inline constexpr u32 kAnimGraphExtended = 1u;  // AnimGraphHeader::flags
inline constexpr u32 kAnimGraphMaxLayers = 4;  // Animator.layer_time holds one playhead per layer
inline constexpr u32 kAnimGraphMaxIK = 8;
inline constexpr u32 kAnimGraphMaxBlend2D = 8;  // points of one 2D blend state
inline constexpr u8 kAnimNoParam = 0xFF;        // a layer's or IK chain's weight_param: none

struct AnimGraphHeader {
    u32 param_count;
    u32 state_count;
    u32 transition_count;
    u32 condition_count;
    u32 blend_point_count;
    u32 event_count;
    u32 start_state;
    u32 flags;  // kAnimGraphExtended
    char params[kAnimGraphMaxParams][24];  // NUL-padded names; Animator.params holds their values in this order
};
static_assert(sizeof(AnimGraphHeader) == 128);

struct AnimGraphState {
    char name[32];
    char clip[32];      // the mesh clip; empty for a blend state
    u32 first_blend;    // blend points (sorted by `at`), when blend_count > 0
    u32 blend_count;
    u32 first_event;
    u32 event_count;
    f32 speed;
    u8 loop;            // 0: plays once and holds its last pose (Animator.finished)
    u8 blend_param;     // parameter index of the 1D blend (a 2D blend: its x parameter)
    u8 blend_2d;        // 1: first_blend / blend_count index the extension's 2D points
    u8 blend_param_y;   // a 2D blend: its y parameter
};
static_assert(sizeof(AnimGraphState) == 88);

struct AnimGraphBlendPoint {
    char clip[32];
    f32 at;  // parameter value where this clip has full weight
    u32 reserved;
};
static_assert(sizeof(AnimGraphBlendPoint) == 40);

enum class AnimCompare : u8 { Greater, Less, GreaterEqual, LessEqual, Equal, NotEqual };

struct AnimGraphCondition {
    u8 param;
    AnimCompare op;
    u8 reserved[2];
    f32 value;
};
static_assert(sizeof(AnimGraphCondition) == 8);

struct AnimGraphTransition {
    u32 from;  // state index, or kAnimAnyState
    u32 to;
    u32 first_condition;
    u32 condition_count;  // all must hold
    f32 fade;             // cross-fade seconds
    u8 on_finish;         // also requires the `from` state's one-shot to have finished
    u8 reserved[3];
};
static_assert(sizeof(AnimGraphTransition) == 24);

struct AnimGraphEvent {
    f32 time;  // seconds into the state's clip
    u32 reserved;
    u64 id;    // StringId (fnv1a64) of the name
    char name[24];
};
static_assert(sizeof(AnimGraphEvent) == 40);

// The extension block (header.flags & kAnimGraphExtended).
struct AnimGraphExtension {
    u32 blend2d_count;
    u32 layer_count;   // <= kAnimGraphMaxLayers
    u32 mask_count;
    u32 ik_count;      // <= kAnimGraphMaxIK
    char root_bone[32];  // root motion: the node whose ground motion moves the entity (empty: none)
    u32 reserved[4];
};
static_assert(sizeof(AnimGraphExtension) == 64);

struct AnimGraphBlendPoint2D {
    char clip[32];
    f32 x, y;  // parameter values where this clip has full weight
};
static_assert(sizeof(AnimGraphBlendPoint2D) == 40);

// A clip played over the state machine's pose, on the bones of its mask (every bone without one):
// an override layer replaces them by its weight, an additive layer adds the clip's motion away
// from its first frame.
struct AnimGraphLayer {
    char name[24];
    char clip[32];
    u32 first_mask;
    u32 mask_count;    // mask bones: each takes its whole subtree; 0 = every bone
    f32 weight;        // times the weight parameter's value when there is one
    f32 speed;
    u8 weight_param;   // kAnimNoParam: none
    u8 additive;
    u8 loop;
    u8 reserved[5];
};
static_assert(sizeof(AnimGraphLayer) == 80);

struct AnimGraphMaskBone {
    char bone[32];
};
static_assert(sizeof(AnimGraphMaskBone) == 32);

enum class AnimIKKind : u8 { TwoBone, LookAt, Foot };

// Inverse kinematics applied after the layers. TwoBone and Foot bend `bone`'s parent and
// grandparent (a hand: elbow and shoulder; a foot: knee and hip) so `bone` reaches its goal;
// LookAt turns `bone` so its `axis` points at its goal. A TwoBone or LookAt goal comes from an
// IKTarget3D entity naming the chain; a Foot goal from the ground below the animated foot.
struct AnimGraphIK {
    char name[24];
    char bone[32];
    char pelvis[32];   // Foot: lowered so the lower foot reaches its ground (empty: none)
    AnimIKKind kind;
    u8 weight_param;   // kAnimNoParam: none
    u8 reserved[2];
    f32 weight;
    f32 axis[3];       // LookAt: the bone-space forward; TwoBone / Foot: the knee's model-space bend direction (0: keep the animated bend)
    f32 limit_deg;     // LookAt: largest turn away from the animated pose
    f32 reach;         // Foot: metres the ground may be above or below the animated foot
    u32 reserved2[3];
};
static_assert(sizeof(AnimGraphIK) == 128);

struct AnimGraphView {
    const AnimGraphHeader* header = nullptr;
    Span<const AnimGraphState> states;
    Span<const AnimGraphTransition> transitions;
    Span<const AnimGraphCondition> conditions;
    Span<const AnimGraphBlendPoint> blend_points;
    Span<const AnimGraphEvent> events;
    const AnimGraphExtension* extension = nullptr;  // null: a graph without the step-5 extras
    Span<const AnimGraphBlendPoint2D> blend2d;
    Span<const AnimGraphLayer> layers;
    Span<const AnimGraphMaskBone> mask_bones;
    Span<const AnimGraphIK> ik;
    StringView root_bone() const { return extension ? text(extension->root_bone, sizeof extension->root_bone) : StringView(); }
    static Result<AnimGraphView> read(Span<const byte> payload);
    u32 find_state(StringView name) const;  // states.size() when absent
    static StringView text(const char* s, usize cap);  // a NUL-padded name
};

// ------------------------------------------------------------------ terrain (CHNK_TERRAIN)
// A heightmap terrain (M-TERRAIN): resolution × resolution u16 height samples, z rows of x columns, row
// 0 at z = -size_z / 2 and column 0 at x = -size_x / 2 of the terrain's own space (centred on the entity
// that places it, Y up). A sample maps 0 -> height_min and 65535 -> height_max metres. Cooked from a
// .mcterrain (terrain_importer.cpp); drawn through render::TerrainLod (mce/render/terrain.h).
//
// Splat layers (M-TERRAIN 2): up to kTerrainMaxLayers materials painted over the terrain by a weight map
// of splat_resolution² samples spanning the same extent as the heights. The weights are stored as two
// planes of RGBA8 (layers 0-3, then 4-7), each splat_resolution² × 4 bytes in the heights' order: the
// layout a 2-layer RGBA8 texture array takes. A layer's weight is relative: the draw normalizes them.
//
// Holes (M-TERRAIN 3): with TerrainFlags::kHoles, a bit per height sample (sample i at bit i % 8 of byte
// i / 8). A triangle with a hole at any corner is neither drawn nor collided with (Jolt's height-field
// rule), so a hole sample opens the triangles around it.

inline constexpr u32 kTerrainMinResolution = 33;
inline constexpr u32 kTerrainMaxResolution = 4097;
inline constexpr u32 kTerrainMaxLayers = 8;

namespace TerrainFlags {
inline constexpr u32 kHoles = 1u << 0;  // holes_offset holds the hole bits
}

namespace TerrainLayerFlags {
inline constexpr u32 kTriplanar = 1u << 0;  // also projected along x and z where the ground is steep
}

struct TerrainLayer {
    u64 material[2];        // .mcmat GUID {hi, lo}; null = a white layer
    f32 tile;               // metres one repeat of the layer's textures covers
    f32 height_strength;    // how far the layer's albedo brightness lifts it over its neighbours (height blend)
    u32 flags;              // TerrainLayerFlags
    u32 reserved;

    Guid material_guid() const { return Guid{material[0], material[1]}; }
};
static_assert(sizeof(TerrainLayer) == 32);

struct TerrainHeader {
    u32 resolution;    // samples per side: 2^n + 1, kTerrainMinResolution..kTerrainMaxResolution
    u32 chunk;         // quads per side of a finest LOD node: a power of two, 8..256, at most resolution - 1
    f32 size[2];       // metres along x and z
    f32 height_min;    // metres at sample 0
    f32 height_max;    // metres at sample 65535 (> height_min)
    u32 flags;         // TerrainFlags
    u32 layer_count;        // splat layers, 0..kTerrainMaxLayers (0: no splat map)
    u32 splat_resolution;   // weight samples per side: 2^n + 1, 33..resolution (0 without layers)
    u32 reserved[7];
    u64 heights_offset;     // u16[resolution * resolution]
    u64 layers_offset;      // TerrainLayer[layer_count]
    u64 splat_offset;       // u8[2][splat_resolution²][4]
    u64 holes_offset;       // u8[(resolution² + 7) / 8] with TerrainFlags::kHoles
};
static_assert(sizeof(TerrainHeader) == 96);

struct TerrainView {
    const TerrainHeader* header = nullptr;
    Span<const u16> heights;
    Span<const TerrainLayer> layers;
    Span<const u8> splat;  // 2 * splat_resolution² * 4 bytes, empty without layers
    Span<const u8> holes;  // the hole bits, empty without TerrainFlags::kHoles

    u32 resolution() const { return header->resolution; }
    u32 splat_resolution() const { return header->splat_resolution; }
    // Metres between two neighbouring samples along x and z.
    f32 spacing_x() const { return header->size[0] / static_cast<f32>(header->resolution - 1); }
    f32 spacing_z() const { return header->size[1] / static_cast<f32>(header->resolution - 1); }
    // The height in metres of sample (x, z).
    f32 sample(u32 x, u32 z) const {
        const f32 t = static_cast<f32>(heights[usize(z) * header->resolution + x]) / 65535.0f;
        return header->height_min + t * (header->height_max - header->height_min);
    }
    // The bilinear height at (x, z) metres in the terrain's space, clamped to its edges.
    f32 height_at(f32 x, f32 z) const;
    // The height of the surface the terrain draws and collides with at (x, z) metres: each quad split
    // along its (x0, z0)-(x1, z1) diagonal into two planar triangles. Clamped to the edges.
    f32 surface_at(f32 x, f32 z) const;
    bool hole(u32 x, u32 z) const {
        if (holes.empty()) return false;
        const usize i = usize(z) * header->resolution + x;
        return (holes[i / 8] >> (i % 8)) & 1u;
    }
    // Layer `layer`'s weight (0..255) at splat sample (x, z).
    u8 weight(u32 x, u32 z, u32 layer) const {
        const usize n = usize(header->splat_resolution) * header->splat_resolution;
        return splat[(layer / 4) * n * 4 + (usize(z) * header->splat_resolution + x) * 4 + layer % 4];
    }
    // Validates the header (resolution 2^n + 1, chunk a power of two, finite sizes) and the sample,
    // layer and weight counts.
    static Result<TerrainView> read(Span<const byte> payload);
};

// `layers` (header.layer_count of them) and `splat` (2 * splat_resolution² * 4 bytes) are empty without
// layers; `holes` ((resolution² + 7) / 8 bytes) is empty without TerrainFlags::kHoles.
void write_terrain(const TerrainHeader& header, Span<const u16> heights, Array<byte>& out, Span<const TerrainLayer> layers = {},
                   Span<const u8> splat = {}, Span<const u8> holes = {});

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

// ------------------------------------------------------------------ timeline (CHNK_TIMELINE)
// One keyed timeline (FR-ENT-110, M-CINEMA 1): tracks of time-stamped keys a TimelinePlayer
// component (kit/timeline.h) samples every tick. A track drives one target: the playing entity or
// one of the player's eight bound entities. Payload layout:
//   TimelineHeader | TimelineTrack[track_count] | TimelineKey[key_count] | string pool
// A track's keys are contiguous and sorted by time. Strings (property paths, clip names, subtitle
// text) are NUL-terminated offsets into the pool; kTimelineNoString means none.
enum class TimelineTrackKind : u8 {
    Property,    // a reflected component field ("Comp.path"), interpolated between keys
    Activation,  // a bool / integer field held at 1 inside the keys' [t, t + duration) ranges, else 0
    Animation,   // sets the target's Animator clip when the playhead crosses a key
    Audio,       // plays a sound (audio::PlaySfx) when the playhead crosses a key
    Camera,      // camera cuts: the key's target camera is primary from its time until the next key
    Event,       // raises TimelineSignal (and a sequence SeqSignal) when the playhead crosses a key
    Subtitle,    // the line shown during [t, t + duration)
    Video,       // the target's VideoPlayer plays the key's video during [t, t + duration) (0: to its end)
};

enum class TimelineEase : u8 { Linear, Step, Smooth, EaseIn, EaseOut };

inline constexpr u8 kTimelineSelf = 0xFF;  // TimelineTrack::target / TimelineKey::target: the playing entity
inline constexpr u32 kTimelineBindings = 8;  // TimelinePlayer::bindings
inline constexpr u32 kTimelineNoString = ~0u;

struct TimelineTrack {
    TimelineTrackKind kind;
    u8 target;         // a binding index or kTimelineSelf
    u8 value_count;    // Property: floats per key (1 to 4; a quaternion is 4 and slerps)
    u8 flags;          // TimelineTrackFlags
    u32 first_key;
    u32 key_count;
    u32 path;          // Property / Activation: "Component.field.path" in the pool
};
static_assert(sizeof(TimelineTrack) == 16);

namespace TimelineTrackFlags {
inline constexpr u8 Quat = 1u << 0;  // Property: the four values are a rotation (slerp, not lerp)
}

struct TimelineKey {
    f32 t;             // seconds from the start
    f32 duration;      // Activation / Subtitle: the range's length
    f32 v[4];          // Property: the value; Animation: v[0] loop (0/1), v[1] fade (< 0 keeps the Animator's); Audio: v[0] gain dB;
                       // Camera: v[0] blend seconds from the previous cut's camera (0 = a hard cut); Video: v[0] gain dB
    u64 id;            // Animation: StringId of the clip; Event: StringId of the signal
    u64 guid[2];       // Audio: the Sfx {hi, lo}; Video: the Video
    u32 text;          // Animation: the clip name; Subtitle: the line
    u32 text2;         // Subtitle: the speaker (kTimelineNoString when none)
    TimelineEase ease; // Property: the curve from this key to the next
    u8 target;         // Camera: the camera's binding index or kTimelineSelf
    u8 pad_[6];
    Guid asset() const { return Guid{guid[0], guid[1]}; }
};
static_assert(sizeof(TimelineKey) == 64);

struct TimelineHeader {
    char name[32];       // NUL-padded
    f32 duration;        // seconds; > 0
    u32 track_count;
    u32 key_count;
    u32 string_bytes;
    u32 reserved[4];
};
static_assert(sizeof(TimelineHeader) == 64);

struct TimelineView {
    const TimelineHeader* header = nullptr;
    Span<const TimelineTrack> tracks;
    Span<const TimelineKey> keys;
    Span<const char> strings;
    StringView name() const;
    StringView string(u32 offset) const;  // empty for kTimelineNoString
    Span<const TimelineKey> keys_of(const TimelineTrack& t) const { return keys.subspan(t.first_key, t.key_count); }
    static Result<TimelineView> read(Span<const byte> payload);
};

// ------------------------------------------------------------------ behavior tree (CHNK_BT)
// One behavior tree (M-AI 1, ADR-0045): its nodes in pre-order and the blackboard keys they read
// and write. An AiBrain component (kit/ai.h) runs it against the fixed tick. Payload layout:
//   BtHeader | BtKey[key_count] | BtNode[node_count] | string pool
// Node 0 is the root. A node's children follow it: the first is node i + 1, each next one starts at
// the previous one's `end`, and the last one's `end` is the parent's. Strings (key and node names,
// task names) are NUL-terminated offsets into the pool; kBtNoString means none.
enum class BtNodeKind : u8 {
    Sequence,  // composites: children in order
    Selector,
    Parallel,  // every child each tick; `op` is the BtPolicy
    Invert,    // decorators: exactly one child
    Succeed,
    Fail,
    Repeat,    // the child again after each success, `a` times in all (0: forever)
    Retry,     // the child again after each failure, `a` attempts in all (0: forever)
    Cooldown,  // fails without running the child for `a` seconds after the child finished
    Timeout,   // fails (and halts the child) when the child runs `a` seconds
    Guard,     // runs the child only when its condition holds on entry (and, with `abort`, while it runs)
    Check,     // leaves: the condition as a result
    Wait,      // runs `a` seconds, then succeeds
    Set,       // writes a blackboard key, succeeds
    Emit,      // raises kit::BtSignal `id`, succeeds
    Task,      // a task registered by name (kit::BtRegistry::register_task)
};

enum class BtKeyType : u8 { Bool, Int, Float, Vec3, Entity };

// Check / Guard: the key against `a` (Bool, Int and Float keys) or whether it is set.
enum class BtOp : u8 { IsSet, NotSet, Eq, Ne, Lt, Le, Gt, Ge };

// Set: `a` (numbers) or `v` (Vec3) assigned, `a` added, the key cleared, `key2` copied, or the
// running entity (Entity keys).
enum class BtSetOp : u8 { Assign, Add, Clear, Copy, Self };

// Guard observers (M-AI 2): the condition is checked again every tick, not only on entry. Self
// halts the guard's running child when the condition stops holding (the guard fails); Lower, on a
// guard that is a selector's child, halts the selector's running later child when it starts to
// hold, and the selector runs the guard instead. Both does the two.
enum class BtAbort : u8 { None, Self, Lower, Both };

// Parallel: All succeeds when every child has and fails on the first failure; One succeeds on the
// first success and fails when every child has.
enum class BtPolicy : u8 { All, One };

inline constexpr u32 kBtMaxNodes = 128;  // kit::AiBrain::nodes
inline constexpr u32 kBtMaxSlots = 32;   // kit::AiBrain::slots: one per Repeat, Retry, Cooldown, Timeout, Wait and Task node
inline constexpr u32 kBtMaxChildren = 63;
inline constexpr u32 kBtMaxDepth = 32;
inline constexpr u32 kBtNumbers = 16;   // kit::Blackboard: Bool, Int and Float keys share these
inline constexpr u32 kBtVectors = 8;
inline constexpr u32 kBtEntities = 8;
inline constexpr u32 kBtMaxKeys = kBtNumbers + kBtVectors + kBtEntities;
inline constexpr u16 kBtNoKey = 0xFFFF;
inline constexpr u8 kBtNoSlot = 0xFF;
inline constexpr u32 kBtNoString = ~0u;

struct BtKey {
    u64 id;            // StringId of the name
    u32 name;          // the name in the pool
    BtKeyType type;
    u8 slot;           // index into the Blackboard array of its type
    u8 has_default;    // 1: `value` is written when the brain starts and the key is not set yet
    u8 pad_;
    f32 value[3];      // the default (numbers in value[0])
    u32 pad2_;
};
static_assert(sizeof(BtKey) == 32);

struct BtNode {
    BtNodeKind kind;
    u8 child_count;
    u8 slot;           // the node's AiBrain slot or kBtNoSlot
    u8 op;             // BtOp (Check, Guard), BtSetOp (Set) or BtPolicy (Parallel)
    u16 end;           // one past the node's last descendant
    u16 key;           // Check, Guard, Set, Task: a blackboard key index or kBtNoKey
    f32 a;             // seconds, a count, or the number compared, assigned or added
    f32 v[3];          // Set: the Vec3 assigned; Task: its vector argument
    u64 id;            // Emit: the signal; Task: the task (StringId)
    u32 name;          // the node's name for the debugger, or kBtNoString
    u32 text;          // Emit / Task: the signal's or task's name
    u16 key2;          // Set (Copy): the key copied
    u8 abort;          // Guard: a BtAbort (0 elsewhere)
    u8 pad_;
    u32 arg;           // Task: its string argument (a clip, a guid, an event, a text), or kBtNoString
};
static_assert(sizeof(BtNode) == 48);

struct BtHeader {
    char name[32];     // NUL-padded
    u32 node_count;    // 1 to kBtMaxNodes
    u32 key_count;
    u32 slot_count;
    u32 string_bytes;
    u32 reserved[4];
};
static_assert(sizeof(BtHeader) == 64);

struct BtView {
    const BtHeader* header = nullptr;
    Span<const BtKey> keys;
    Span<const BtNode> nodes;
    Span<const char> strings;
    StringView name() const;
    StringView string(u32 offset) const;  // empty for kBtNoString
    // Checks the tree's shape too: child links inside the array, depth, slots and key types.
    static Result<BtView> read(Span<const byte> payload);
};
void write_behavior_tree(const BtHeader& header, Span<const BtKey> keys, Span<const BtNode> nodes, Span<const char> strings,
                         Array<byte>& out);

// ------------------------------------------------------------------ 2D skeleton (CHNK_SKEL2D)
// A 2D cut-out skeleton (M-ANIM2D 1): a bone tree and the slots (textured quads) drawn on its
// bones. kit::Skeleton2D (kit/skeleton2d.h) poses and draws one. Payload layout:
//   Skeleton2DHeader | Skeleton2DBone[bone_count] | Skeleton2DSlot[slot_count] | string pool
// Bones are topologically ordered: a bone's parent comes before it (bone 0 is a root). Slots are
// in draw order (the first is drawn first, behind the rest). Units: metres and radians, Y up.
// Names are NUL-terminated offsets into the pool. Reserved fields are zero in version 1; later
// steps (clips, mesh skinning, IK) extend the format through them and the minor version.
inline constexpr u32 kSkeleton2DMaxBones = 64;   // kit::Skeleton2D holds one pose per bone
inline constexpr u32 kSkeleton2DMaxSlots = 256;
inline constexpr u16 kSkeleton2DNoParent = 0xFFFF;

enum class Skeleton2DSlotKind : u8 {
    Quad,  // a texture rect stretched to `size` (version 1's only kind; meshes come later)
};

struct Skeleton2DBone {
    u32 name;          // string pool offset
    u16 parent;        // a lower bone index, or kSkeleton2DNoParent
    u16 flags;         // reserved (0)
    f32 x, y;          // rest pose: metres in the parent bone's space
    f32 rotation;      // rest pose: radians, counter-clockwise
    f32 scale_x, scale_y;
    f32 length;        // metres along the bone's +x (gizmos, IK)
    u32 reserved[4];
};
static_assert(sizeof(Skeleton2DBone) == 48);

struct Skeleton2DSlot {
    u32 name;               // string pool offset
    u16 bone;               // the bone it rides
    Skeleton2DSlotKind kind;
    u8 flags;               // reserved (0)
    u64 texture[2];         // texture GUID {hi, lo}; zero = a solid quad in `tint`
    f32 rect[4];            // u0, v0, u1, v1 of the texture; v0 = top edge
    f32 size[2];            // metres
    f32 pivot[2];           // normalized; (0, 0) = bottom-left
    f32 offset[2];          // metres in the bone's space: where the pivot sits
    f32 rotation;           // radians in the bone's space
    f32 tint[4];            // linear RGBA
    u32 reserved[3];
    Guid texture_guid() const { return Guid{texture[0], texture[1]}; }
};
static_assert(sizeof(Skeleton2DSlot) == 96);

struct Skeleton2DHeader {
    char name[32];  // NUL-padded
    u32 bone_count;  // 1 to kSkeleton2DMaxBones
    u32 slot_count;  // 0 to kSkeleton2DMaxSlots
    u32 string_bytes;
    u32 flags;       // reserved (0)
    u32 reserved[4];
};
static_assert(sizeof(Skeleton2DHeader) == 64);

struct Skeleton2DView {
    const Skeleton2DHeader* header = nullptr;
    Span<const Skeleton2DBone> bones;
    Span<const Skeleton2DSlot> slots;
    Span<const char> strings;
    StringView name() const;
    StringView string(u32 offset) const;
    // The index of the bone named `name`, or -1.
    i32 find_bone(StringView name) const;
    // Checks counts, parent order, slot bones, string offsets and that every number is finite.
    static Result<Skeleton2DView> read(Span<const byte> payload);
};

// ------------------------------------------------------------------ shader (CHNK_SHD)
// One GLSL 4.5 stage (assets.md §5.2, FR-AST-060): the validated source text, the variant table
// (every combination of the boolean defines its `// @variant NAME` header lines declare), and per
// variant the SPIR-V the cook compiled (OpenGL client semantics) plus a reflection list of the
// resources it uses. GL recompiles the source at load (`compose`); SPIR-V serves backends that
// translate from it and tools. Payload layout:
//   ShaderHeader | ShaderVariant[variant_count] | ShaderResource[resource_count]
//   | source text (source_size bytes, padded to 4) | SPIR-V words
enum class ShaderStageKind : u8 { Vertex, Fragment, Compute };
enum class ShaderResourceKind : u8 { UniformBlock, StorageBlock, Sampler, Input, Output };

inline constexpr u32 kMaxShaderDefines = 6;  // 64 variants at most

struct ShaderHeader {
    ShaderStageKind stage;
    u8 define_count;
    u16 variant_count;
    u32 resource_count;   // all variants
    u32 source_size;      // bytes, without padding
    u32 reserved;
    char defines[kMaxShaderDefines][24];  // NUL-padded; bit i of a variant mask = defines[i]
};
static_assert(sizeof(ShaderHeader) == 160);

struct ShaderVariant {
    u32 mask;             // which defines are 1 (the others are 0)
    u32 spirv_offset;     // bytes from the payload start
    u32 spirv_words;
    u32 first_resource;
    u32 resource_count;
    u32 reserved[3];
};
static_assert(sizeof(ShaderVariant) == 32);

struct ShaderResource {
    char name[32];        // NUL-padded (block or variable name, truncated)
    ShaderResourceKind kind;
    u8 pad_;
    u16 binding;          // blocks and samplers: binding; inputs/outputs: location
    u32 size;             // uniform/storage block bytes (0 otherwise)
};
static_assert(sizeof(ShaderResource) == 40);

struct ShaderView {
    const ShaderHeader* header = nullptr;
    Span<const ShaderVariant> variants;
    Span<const ShaderResource> resources;
    StringView source;  // as authored

    StringView define(u32 i) const;
    const ShaderVariant* find(u32 mask) const;
    Span<const u32> spirv(const ShaderVariant& v) const;
    Span<const ShaderResource> resources_of(const ShaderVariant& v) const;
    // The text a GL driver compiles for `mask`: the source with "#define NAME 0|1" lines after
    // its #version line (the same text the cook validated).
    void compose(u32 mask, Array<char>& out) const;
    static Result<ShaderView> read(Span<const byte> payload);
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
//
// Skins (v0.5, FR-R3D-060, optional): a mesh with MeshFlags::Skinned carries a skin section at
// MeshHeader::skin_offset — the glTF skins (joint nodes + inverse bind matrices + per-joint bind
// bounds), one MeshSkinVertex per vertex (parallel to the vertex stream) and a skin index per
// submesh (-1 = rigid, placed by its node as before). Joints are ordinary nodes, so the node clips
// above animate skeletons too. A mesh without the flag is exactly the static layout (minor 0).
//
// LODs (M-KIT3D, #158, optional): with MeshFlags::Lods a submesh may carry up to
// kMaxMeshLods - 1 coarser levels of itself, cooked by meshoptimizer's simplifier or imported from
// `<node>_LOD<n>` nodes. Their index ranges sit in the same index stream; a generated level uses
// the submesh's own vertices, an authored one a vertex range of its own quantized with the
// submesh's center / extent / UV rectangle (one dequantization for every level).
// MeshSubmesh::reserved[0] is the submesh's level count beyond the base, reserved[1] its first
// entry in the MeshLod table, which follows the string pool (the next CHUNK_ALIGN boundary after
// strings_offset + strings_size) and is as long as the largest first + count.

namespace MeshFlags {
enum : u32 {
    Index32 = 1u << 0,
    Skinned = 1u << 1,  // a MeshSkinHeader at MeshHeader::skin_offset (chunk minor 1)
    Lods = 1u << 2,     // a MeshLod table after the strings (chunk minor 2)
    Ragdoll = 1u << 3,  // MeshJoint's ragdoll fields hold the sidecar's per-bone overrides (chunk minor 3)
    Materials = 1u << 4,  // a MaterialExt per submesh, at MeshSubmesh::reserved2[0] (chunk minor 4)
};
}

inline constexpr u32 kMaxMeshLods = 4;  // levels per submesh, the base included

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
    u32 skin_offset;   // MeshSkinHeader, when MeshFlags::Skinned (0 otherwise)
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
    u32 reserved2[2];        // [0]: with MeshFlags::Materials, the payload offset of this submesh's MaterialExt
};
static_assert(sizeof(MeshSubmesh) == 192);

// One coarser level of a submesh: indices relative to `base_vertex` (the submesh's own for a
// generated level), drawn with the submesh's dequantization and material.
struct MeshLod {
    u32 first_index;
    u32 index_count;
    u32 base_vertex;
    u32 vertex_count;
    f32 error;         // geometric deviation from the base, node-local units (screen-size selection)
    u32 reserved[3];
};
static_assert(sizeof(MeshLod) == 32);

// The LOD fields of a submesh (MeshSubmesh::reserved, MeshFlags::Lods).
inline u32 submesh_lod_count(const MeshSubmesh& s) { return s.reserved[0]; }
inline u32 submesh_lod_first(const MeshSubmesh& s) { return s.reserved[1]; }

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

// Skinning (MeshFlags::Skinned). Up to 4 influences per vertex: `joints` index the submesh's
// skin's joint list (every slot, weighted or not: unused slots name joint 0), `weights` are unorm8
// summing to 255 (all zero = not skinned).
struct MeshSkinVertex {
    u8 joints[4];
    u8 weights[4];
};
static_assert(sizeof(MeshSkinVertex) == 8);

inline constexpr u32 kMaxSkinJoints = 256;  // u8 joint indices

struct MeshSkin {
    u32 first_joint;   // into the joint table
    u32 joint_count;   // 1..kMaxSkinJoints
    u32 reserved[2];
};
static_assert(sizeof(MeshSkin) == 16);

// Per-bone ragdoll overrides (MeshFlags::Ragdoll, M-GAMEPLAY3D step 3): which fields of a
// MeshJoint's ragdoll_* the sidecar set. Without the flag every ragdoll_* field is zero.
namespace MeshRagdoll {
enum : u32 {
    Exclude = 1u << 0,  // no body: the bone rides with its parent's
    Include = 1u << 1,  // a body even when shorter than the ragdoll's minimum bone length
    Radius = 1u << 2,   // ragdoll_radius (metres) replaces the automatic radius
    Swing = 1u << 3,    // ragdoll_swing (degrees): how far the bone swings from its parent
    Twist = 1u << 4,    // ragdoll_twist (degrees): how far it twists either way
    Mass = 1u << 5,     // ragdoll_mass: a weight on the bone's share of the mass
};
}

struct MeshJoint {
    u32 node;               // the node whose model-space matrix drives the joint
    u32 ragdoll_flags;      // MeshRagdoll
    f32 ragdoll_radius;
    f32 ragdoll_swing;
    f32 inverse_bind[16];   // column-major: bind space -> joint space
    f32 bounds_min[3];      // bind-space AABB of the vertices this joint influences; min > max = none
    f32 bounds_max[3];
    f32 ragdoll_twist;
    f32 ragdoll_mass;
};
static_assert(sizeof(MeshJoint) == 112);

struct MeshSkinHeader {
    u32 skin_count;
    u32 joint_count;
    u32 vertex_count;   // == MeshHeader::vertex_count
    u32 submesh_count;  // == MeshHeader::submesh_count
    u64 skins_offset;
    u64 joints_offset;
    u64 vertices_offset;  // MeshSkinVertex[vertex_count]
    u64 submesh_skins_offset;  // i32[submesh_count]: skin index, -1 = rigid
};
static_assert(sizeof(MeshSkinHeader) == 48);

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
    // Skin section (empty unless skinned()).
    Span<const MeshSkin> skins;
    Span<const MeshJoint> joints;
    Span<const MeshSkinVertex> skin_vertices;
    Span<const i32> submesh_skins;
    // LOD table (empty unless MeshFlags::Lods).
    Span<const MeshLod> lods;
    // One per submesh with MeshFlags::Materials, else empty.
    Span<const MaterialExt> materials;
    MaterialExt material_ext(u32 s) const { return materials.empty() ? MaterialExt{} : materials[s]; }

    bool skinned() const { return header && (header->flags & MeshFlags::Skinned); }
    // The coarser levels of submesh `s` (empty: none).
    Span<const MeshLod> submesh_lods(u32 s) const {
        const MeshSubmesh& sm = submeshes[s];
        if (lods.empty() || submesh_lod_count(sm) == 0) return {};
        return lods.subspan(submesh_lod_first(sm), submesh_lod_count(sm));
    }
    bool index32() const { return header && (header->flags & MeshFlags::Index32); }
    u32 index(u32 i) const;
    StringView node_name(u32 node) const;
    StringView clip_name(u32 clip) const;
    // Clip index by name, or clips.size() when absent.
    u32 find_clip(StringView name) const;
    // Validates every offset, every index against its submesh's vertex range, node parents,
    // channel ranges and the skin section (joint nodes, per-vertex joint indices against the
    // submesh's skin): the payload is untrusted bytes and its indices reach the GPU.
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
    // Optional skin section: written (and MeshFlags::Skinned set) when `skins` is non-empty;
    // `skin_vertices` then has one entry per vertex and `submesh_skins` one per submesh.
    Span<const MeshSkin> skins;
    Span<const MeshJoint> joints;
    Span<const MeshSkinVertex> skin_vertices;
    Span<const i32> submesh_skins;
    // Optional LOD table: written (and MeshFlags::Lods set) when non-empty; submeshes name their
    // range of it in reserved[0] (count) and reserved[1] (first).
    Span<const MeshLod> lods;
    // Optional material extensions: written (and MeshFlags::Materials set) when non-empty, one per submesh.
    Span<const MaterialExt> materials;
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

// ------------------------------------------------------------------ mod info (CHNK_MOD)
// A mod pack's identity (FR-RUN-090, ADR-0034): written by the cook into a mod project's pack,
// right after the manifest, with a zero GUID (it belongs to the pack, not to an asset). Five
// strings in one blob; the reader checks bounds only, the meaning of each field (id charset,
// game match) is mods.h's business.

struct ModInfoString {
    u32 offset;  // into the string blob that follows the header
    u32 size;
};

struct ModInfoHeader {
    ModInfoString id;           // [a-z0-9_.-], 1..64 bytes: names the mod in load_order.txt
    ModInfoString name;         // display name
    ModInfoString version;      // the mod's own version, free text ("1.2.0")
    ModInfoString game;         // the game it targets (a PlayerDesc::mods_game_id); empty = any
    ModInfoString description;
    u32 strings_size;
    u32 reserved[5];
};
static_assert(sizeof(ModInfoHeader) == 64);

struct ModInfoData {
    StringView id;
    StringView name;
    StringView version;
    StringView game;
    StringView description;
};

struct ModInfoView {
    ModInfoData data;  // views into the payload
    static Result<ModInfoView> read(Span<const byte> payload);
};
void write_mod_info(const ModInfoData& info, Array<byte>& out);

}  // namespace mce::asset
