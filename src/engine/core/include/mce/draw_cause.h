#pragma once
// Why a main-pass draw call was not folded into an instanced run (M-CONTENT 2). The renderer counts its
// scene draws by cause (Forward3DStats::draw_causes), the player copies the counts into every perf frame
// (perf::Counters::draw_causes), and perf_run / `mce perf` list them with the hint of each cause.
// The names are the stable spelling used in .mcperf files and reports; the order is the index.

#include "mce/types.h"

namespace mce {

enum class DrawCause : u8 {
    Material,    // another draw uses the same mesh, but with a different material, tint or emissive tint
    UniqueMesh,  // the mesh is drawn once: nothing to share a run with
    Skinned,     // a skinned mesh (its joints go through a palette)
    Terrain,     // a terrain node
    Moving,      // moved since last frame while TAA or motion blur reads the previous transform
    Blended,     // alpha blended or transmissive: sorted back to front after the opaques
    Disabled,    // automatic instancing is off (InstancingSettings::enabled, -r.instancing 0, no pipelines)
    Budget,      // a run the frame's per-copy upload budget had no room for
    Other,       // a group shorter than InstancingSettings::min_count
    Count
};

inline constexpr u32 kDrawCauseCount = static_cast<u32>(DrawCause::Count);

constexpr const char* draw_cause_name(DrawCause c) {
    switch (c) {
        case DrawCause::Material: return "material";
        case DrawCause::UniqueMesh: return "unique_mesh";
        case DrawCause::Skinned: return "skinned";
        case DrawCause::Terrain: return "terrain";
        case DrawCause::Moving: return "moving";
        case DrawCause::Blended: return "blended";
        case DrawCause::Disabled: return "instancing_off";
        case DrawCause::Budget: return "budget";
        case DrawCause::Other: return "other";
        case DrawCause::Count: break;
    }
    return "?";
}

// What to change, in one line.
constexpr const char* draw_cause_hint(DrawCause c) {
    switch (c) {
        case DrawCause::Material:
            return "copies of one mesh differ in material, tint or emissive tint: give them one material and one tint "
                   "(an atlas texture instead of many materials) so they share a run";
        case DrawCause::UniqueMesh:
            return "meshes drawn once cannot instance: reuse one mesh for repeated props, or merge static meshes "
                   "into one mesh in the importer (a static batch)";
        case DrawCause::Skinned:
            return "skinned meshes always draw singly: use a rigid mesh for props and crowds in the distance";
        case DrawCause::Terrain:
            return "terrain draws one node at a time: fewer, larger terrain nodes";
        case DrawCause::Moving:
            return "moving copies are drawn singly while TAA or motion blur is on: turn both off, or keep "
                   "background props still";
        case DrawCause::Blended:
            return "alpha-blended and transmissive draws sort back to front and never share a run: use alpha mask "
                   "or opaque materials where you can";
        case DrawCause::Disabled:
            return "automatic instancing is off: remove -r.instancing 0 / InstancingSettings::enabled = false";
        case DrawCause::Budget:
            return "the instancing upload budget ran out: raise Forward3DDesc::instancing_budget (and "
                   "geometry_ring_bytes) or draw fewer copies";
        case DrawCause::Other:
            return "a group of identical copies shorter than InstancingSettings::min_count: lower min_count or add "
                   "copies";
        case DrawCause::Count: break;
    }
    return "";
}

}  // namespace mce
