#pragma once
// TerrainLod — a heightmap terrain (asset::TerrainView, M-TERRAIN 1) drawn as a quadtree of grid meshes
// with CDLOD-style selection.
//
// The terrain is cut into nodes. A finest node (level 0) covers chunk × chunk quads of samples; a node
// of level L covers 2 × 2 nodes of level L - 1 with the same chunk × chunk quads at twice their spacing,
// up to one root over the whole terrain. Every node keeps its bounds and its geometric error: the
// largest height difference between the full-resolution surface and its own grid (never less than its
// children's). select() walks down from the root and keeps the coarsest node whose error, seen at the
// node's nearest point to the camera, stays under `pixel_error` pixels, dropping nodes outside the view
// that are also farther than the shadow reach (a hill behind the camera still casts its shadow).
//
// Holes (M-TERRAIN 3): a triangle with a hole sample at a corner is not drawn, as Jolt's height field
// does not collide with it; a node holding a hole always splits down to level 0, so holes keep their
// shape at any distance.
//
// Neighbours of different levels meet with skirts: a strip hanging down from each edge a node shares
// with another node, deeper than the error of the coarser neighbours it can meet, so the gap between
// two grids never shows what is behind the terrain. Edges on the terrain's border get none.
//
// build_mesh() writes one node as a cooked mesh payload (the CHNK_MESH layout), so a MeshStore uploads
// it and Scene3D draws it as any other mesh: shadow cascades, every pass, on GL and SDL GPU alike.
//
//   TerrainLod lod;
//   lod.init(view);                                 // copies the heights
//   Array<u32> nodes;
//   lod.select(scene.view(), {.world = entity_world, .pixel_error = 2.0f}, nodes);
//   for (u32 n : nodes) {                           // upload a node the first time it is drawn
//       lod.build_mesh(n, 0.0f, blob);
//       meshes.upload(key_of(n), 1, blob);
//   }
//
// Terrain space: Y up, metres, centred on the terrain (sample (0, 0) at x = -size_x / 2, z = -size_z / 2).

#include "mce/asset/formats.h"
#include "mce/containers/array.h"
#include "mce/math/math.h"
#include "mce/render/scene3d.h"
#include "mce/render/texture_store.h"

namespace mce::render {

struct TerrainNode {
    math::aabb3 bounds;  // terrain space, the grid's surface (skirts excluded)
    f32 error = 0.0f;    // metres: the full-resolution surface against this node's grid
    f32 skirt = 0.0f;    // metres the skirts hang below the surface
    u32 level = 0;       // 0 = finest
    u32 x = 0, z = 0;    // node coordinates within its level
    bool holed = false;  // holds a hole sample: drawn only at level 0 (M-TERRAIN 3)
};

struct TerrainSelectParams {
    math::mat4 world = math::mat4::identity();  // terrain space -> world (the entity's transform)
    f32 pixel_error = 2.0f;                     // the most a node's error may cover on screen
    f32 shadow_reach = 0.0f;                    // metres around the camera kept even off-screen
};

class TerrainLod {
public:
    static constexpr u32 kMaxLevels = 10;  // 4096 / 8 = 512 finest nodes per side

    // Copies the terrain's heights and builds the quadtree (bounds and errors of every node).
    ResultV init(const asset::TerrainView& terrain);
    bool empty() const { return nodes_.empty(); }

    u32 resolution() const { return resolution_; }
    u32 chunk() const { return chunk_; }
    math::vec2 size() const { return size_; }
    u32 levels() const { return levels_; }  // the root is level levels() - 1
    u32 node_count() const { return static_cast<u32>(nodes_.size()); }
    const TerrainNode& node(u32 index) const { return nodes_[index]; }
    // Index of the node at (level, x, z) (x, z < 2^(levels - 1 - level)).
    u32 node_index(u32 level, u32 x, u32 z) const { return level_first_[level] + z * level_side_[level] + x; }
    u32 root() const { return node_index(levels_ - 1, 0, 0); }
    // Triangles one node draws: its grid and its skirts, less what its holes cut.
    u32 node_triangles(u32 index) const;

    // The height in metres of sample (x, z), and the bilinear height at (x, z) metres in terrain space.
    f32 sample(u32 x, u32 z) const { return height_min_ + static_cast<f32>(heights_[usize(z) * resolution_ + x]) * height_scale_; }
    f32 height_at(f32 x, f32 z) const;
    // The height of the drawn (level 0) and collided surface at (x, z) metres: asset::TerrainView::surface_at.
    f32 surface_at(f32 x, f32 z) const;
    bool hole(u32 x, u32 z) const {
        if (holes_.empty()) return false;
        const usize i = usize(z) * resolution_ + x;
        return (holes_[i / 8] >> (i % 8)) & 1u;
    }
    // The surface normal at sample (x, z) (central differences of the full-resolution heights).
    math::vec3 normal_at(u32 x, u32 z) const;

    // The nodes to draw for `view` (see the header comment), in tree order. Appends to `out`.
    void select(const View3D& view, const TerrainSelectParams& params, Array<u32>& out) const;
    // Node `index` as a cooked mesh payload: one submesh with an identity node, positions in terrain
    // space, normals from the full-resolution heights, UVs of `uv_tile` metres per repeat (0: one
    // repeat over the whole terrain). Its material is a plain grey the caller usually overrides.
    void build_mesh(u32 index, f32 uv_tile, Array<byte>& out) const;

private:
    void compute_errors();
    u32 stride(u32 level) const { return 1u << level; }

    Array<u16> heights_;
    Array<u8> holes_;  // the asset's hole bits, empty without holes
    Array<TerrainNode> nodes_;
    u32 level_first_[kMaxLevels] = {};
    u32 level_side_[kMaxLevels] = {};
    u32 levels_ = 0;
    u32 resolution_ = 0;
    u32 chunk_ = 0;
    math::vec2 size_{0.0f, 0.0f};
    math::vec2 spacing_{0.0f, 0.0f};
    f32 height_min_ = 0.0f;
    f32 height_scale_ = 0.0f;  // metres per sample unit
};

// One map kind (albedo, normal or ORM) of a terrain's splat layers as one texture array (M-TERRAIN 2):
// array layer i holds the cooked texture of terrain layer i. The textures must agree on format, color
// space, size and mip count; the first one present sets them and any other is left out with a warning
// (its bit stays clear in `present`, so the shader keeps that layer's flat values). Missing layers stay
// zero-filled. `upload` points into `pixels` and `mips`: keep the struct alive until it is uploaded.
struct LayerArray {
    Array<byte> pixels;
    Array<TextureMipUpload> mips;
    TextureUpload upload;
    u32 present = 0;  // bit i: layer i's texture is in the array
};
// False (and `out.present` 0) when no layer has a usable texture.
bool compose_layer_array(Span<const asset::TextureView* const> layers, LayerArray& out, const char* debug_name = "terrain.layers",
                         bool widen_bc4 = false);  // widen_bc4: a BC4 layer beside a BC5 one joins as BC5 (parallax height)

// The weight map of a terrain with splat layers as a 2-layer RGBA8 array upload (pointing into the view).
TextureUpload splat_upload(const asset::TerrainView& terrain, TextureMipUpload& mip);

// Foliage on a terrain (M-TERRAIN 5): world matrices of copies standing on the surface, `density` per
// square metre (terrain space) where splat layer `layer` has full weight, thinned in proportion to the
// weight elsewhere (kAnyLayer: everywhere). One candidate per cell of a 1 / sqrt(density) grid, jittered
// inside its cell, kept with probability weight / 255; none on a quad with a hole corner or steeper than
// `max_slope_deg`. Each copy gets a random yaw (`random_yaw`) and a uniform scale in [scale_min,
// scale_max]. A cell's draw depends only on `seed` and the cell, so repainting one area leaves the copies
// elsewhere where they were. The terrain stands at `position` / `rotation` / `scale` (the entity's pose);
// copies take its rotation and stay upright in it. At most `max_count` copies (the grid coarsens past
// four candidates per copy allowed).
struct TerrainScatter {
    static constexpr u32 kAnyLayer = 8;
    u32 layer = 0;
    f32 density = 1.0f;
    u32 max_count = 100000;
    u32 seed = 1;
    f32 scale_min = 0.8f, scale_max = 1.2f;
    f32 max_slope_deg = 90.0f;
    bool random_yaw = true;
    math::vec3 position{0.0f, 0.0f, 0.0f};
    math::quat rotation;
    math::vec3 scale{1.0f, 1.0f, 1.0f};
};
void scatter_on_terrain(const asset::TerrainView& terrain, const TerrainScatter& scatter, Array<math::mat4>& out);

}  // namespace mce::render
