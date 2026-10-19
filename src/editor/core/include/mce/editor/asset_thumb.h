#pragma once
// Asset thumbnails (M-THUMBS 1): a small RGBA picture of an asset from CPU code alone, no GPU. A pure
// function of the asset's bytes: the same asset gives the same pixels, so a thumbnail can be cached by
// GUID and content hash and pinned by a test.
//
//   Image    box-filtered down to fit `size` x `size`, kept in proportion, alpha over a checker
//   Audio    the waveform (min and max per column) on a dark tile, mono mix of the channels
//   Mesh     flat-shaded triangles from a fixed three-quarter view, z-buffered, scaled to fit
//   Material a lit sphere (M-ASSETLOOK 1): base colour (times the base colour texture's average), metallic,
//            roughness, emissive; a see-through base colour or "blend" shows the checker through it
//   Particles the preset's emitter run for 1.5 s from a fixed seed (burst, rate, area, direction, spread, speed,
//            gravity, drag, size and colour over life), the live particles at ten instants drawn as squares, older
//            ones fainter (M-ASSETLOOK 2)
//   Other    a plain tile in a colour taken from the asset type
//
// Every thumbnail is exactly `size` x `size` pixels.

#include "mce/asset/container.h"
#include "mce/cook/audio.h"
#include "mce/cook/image.h"
#include "mce/types.h"

namespace mce::asset {
struct MeshView;
}

namespace mce::editor {

class EditorProject;
struct ProjectAsset;

constexpr u32 kThumbMin = 16;
constexpr u32 kThumbMax = 512;

void thumb_from_image(const cook::Image& src, u32 size, cook::Image& out);
void thumb_from_pcm(const cook::PcmClip& clip, u32 size, cook::Image& out);
// Returns false (and draws the plain tile) when the mesh has no triangles.
bool thumb_from_mesh(const asset::MeshView& mesh, u32 size, cook::Image& out);
void thumb_tile(asset::ChunkType type, u32 size, cook::Image& out);

// The scalar look of a material, in the units of the .mcmat file (linear colour).
struct MaterialSwatch {
    f32 base[4] = {1, 1, 1, 1};
    f32 metallic = 0;
    f32 roughness = 1;
    f32 emissive[3] = {0, 0, 0};
    bool blend = false;  // alpha_mode "blend": the base alpha lets the checker show through
    bool unlit = false;  // drawn with the base colour as it is
};
// A sphere lit by one key light and a sky/ground ambient, over a dark checker. Plain Blinn-Phong with a GGX-like
// exponent from the roughness, a Fresnel-ish F0 from the metallic: it tells materials apart, it is not the renderer.
void thumb_from_material(const MaterialSwatch& m, u32 size, cook::Image& out);

// A particle preset's emitter parameters, in the units of the .mcpart file (angles in radians here).
struct ParticleSwatch {
    f32 rate = 0;
    u32 burst = 0;
    u32 max_particles = 256;
    f32 duration = 0;  // seconds of emission; 0 = emit for as long as `rate` is positive
    f32 lifetime[2] = {1, 1};
    f32 speed[2] = {1, 1};
    f32 direction = 1.5707963f;  // radians, 0 = +X, counter-clockwise (y up)
    f32 spread = 0;
    f32 gravity[2] = {0, 0};
    f32 drag = 0;
    f32 size[2] = {0.25f, 0.25f};  // start, end (metres)
    f32 area[2] = {0, 0};          // half extents of the emission box
    f32 color_start[4] = {1, 1, 1, 1};
    f32 color_end[4] = {1, 1, 1, 1};
    bool additive = false;
};
// A pure function of the parameters (a fixed seed, 90 fixed steps of 1/60 s). The picture is fitted to the particles'
// extent, at least half a metre across, y up; a preset that emits nothing gives the dark background.
void thumb_from_particles(const ParticleSwatch& p, u32 size, cook::Image& out);

// The thumbnail of a catalog asset: textures from the source PNG, audio from the source WAV, meshes
// from the mounted cooked chunk; everything else (or anything that does not read) a plain tile.
// `size` is clamped to kThumbMin..kThumbMax. Returns true when the picture shows the asset itself.
bool render_thumbnail(EditorProject& project, const ProjectAsset& asset, u32 size, cook::Image& out);

}  // namespace mce::editor
