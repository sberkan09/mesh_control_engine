#pragma once
// Asset thumbnails (M-THUMBS 1): a small RGBA picture of an asset from CPU code alone, no GPU. A pure
// function of the asset's bytes: the same asset gives the same pixels, so a thumbnail can be cached by
// GUID and content hash and pinned by a test.
//
//   Image    box-filtered down to fit `size` x `size`, kept in proportion, alpha over a checker
//   Audio    the waveform (min and max per column) on a dark tile, mono mix of the channels
//   Mesh     flat-shaded triangles from a fixed three-quarter view, z-buffered, scaled to fit
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

// The thumbnail of a catalog asset: textures from the source PNG, audio from the source WAV, meshes
// from the mounted cooked chunk; everything else (or anything that does not read) a plain tile.
// `size` is clamped to kThumbMin..kThumbMax. Returns true when the picture shows the asset itself.
bool render_thumbnail(EditorProject& project, const ProjectAsset& asset, u32 size, cook::Image& out);

}  // namespace mce::editor
