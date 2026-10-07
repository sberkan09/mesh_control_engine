#pragma once
// CPU clipping of UI triangles to a clip rectangle (the ImGui renderer's scissor).
//
// mce_rhi has no scissor state at M0, so the editor clips ImGui's geometry on the CPU before it
// is uploaded: triangles inside the rect pass through untouched, triangles outside are dropped,
// and the few that straddle an edge (glyphs and rows at a scrolled window's border) are cut with
// Sutherland-Hodgman, interpolating position, UV and color. The result matches a scissor test up
// to rasterization rounding. ImGui-free so it is unit-testable headless.

#include "mce/containers/array.h"
#include "mce/types.h"

namespace mce::editor {

// Same layout as ImDrawVert (the renderer static_asserts it).
struct ClipVertex {
    f32 x, y;
    f32 u, v;
    u32 rgba;
};

struct ClipRect {
    f32 x0, y0, x1, y1;  // x0 <= x1, y0 <= y1
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};

// Appends the triangles `indices` (into `verts`) clipped to `rect` as 32-bit indices to
// `out_indices`. `verts` must already be copied into `out_vertices` starting at `out_base`:
// unclipped triangles reference those copies; cut ones append new vertices.
// Returns the number of indices appended.
u32 clip_triangles(Span<const ClipVertex> verts, u32 out_base, Span<const u16> indices, ClipRect rect,
                   Array<ClipVertex>& out_vertices, Array<u32>& out_indices);

}  // namespace mce::editor
