#pragma once
// The .mcanim source format (M-DATA-REACH 7): read by the `animgraph` importer, written by tools that
// edit a graph (the editor's Animation Graph panel, M-CINEMA step 3). The format is documented in
// animgraph_importer.cpp and user guide 8.
//
// AnimGraphDocument is the source as data. Writing emits only what differs from the defaults, so
// reading what was written gives back the same document, and cooking it the same CHNK_ANIMG bytes.
// A state's `pos` is where the editor draws it; the cook ignores it.

#include "mce/asset/formats.h"
#include "mce/asset/guid.h"
#include "mce/cook/diagnostics.h"
#include "mce/cook/json.h"

namespace mce::cook {

inline constexpr u32 kAnimGraphFormatVersion = 2;  // 1 still reads; written when the graph uses no step-5 extras
inline constexpr u32 kAnimGraphAny = asset::kAnimAnyState;  // a transition's `from`: any state

const char* anim_compare_name(asset::AnimCompare op);  // ">", "<", ...

struct AnimGraphDocPoint {
    String clip;
    f32 at = 0.0f;
};

struct AnimGraphDocPoint2D {
    String clip;
    f32 x = 0.0f, y = 0.0f;
};

struct AnimGraphDocEvent {
    f32 time = 0.0f;
    String name;
};

struct AnimGraphDocState {
    String name;
    String clip;                       // a one-clip state (empty: a blend state)
    u8 blend_param = 0;                // a blend state: parameter index
    Array<AnimGraphDocPoint> points;   // a 1D blend state: >= 2, increasing `at`
    bool blend_2d = false;             // a 2D blend state ("blend2d"): blend_param is x, blend_param_y is y
    u8 blend_param_y = 0;
    Array<AnimGraphDocPoint2D> points2d;  // a 2D blend state: 3 to asset::kAnimGraphMaxBlend2D points
    bool loop = true;
    f32 speed = 1.0f;
    Array<AnimGraphDocEvent> events;
    f32 pos[2] = {0.0f, 0.0f};         // editor layout
    bool has_pos = false;

    bool is_blend() const { return clip.empty(); }
};

struct AnimGraphDocCondition {
    u8 param = 0;
    asset::AnimCompare op = asset::AnimCompare::Greater;
    f32 value = 0.0f;
};

struct AnimGraphDocTransition {
    u32 from = kAnimGraphAny;  // state index, or kAnimGraphAny
    u32 to = 0;
    Array<AnimGraphDocCondition> when;  // all must hold
    f32 fade = 0.2f;
    bool on_finish = false;
};

struct AnimGraphDocLayer {
    String name;
    String clip;
    Array<String> mask;   // bones (each with its subtree); empty: every bone
    f32 weight = 1.0f;
    u8 weight_param = asset::kAnimNoParam;
    f32 speed = 1.0f;
    bool additive = false;
    bool loop = true;
};

struct AnimGraphDocIK {
    String name;
    asset::AnimIKKind kind = asset::AnimIKKind::TwoBone;
    String bone;
    String pelvis;                         // Foot only
    f32 weight = 1.0f;
    u8 weight_param = asset::kAnimNoParam;
    f32 axis[3] = {0.0f, 0.0f, 0.0f};      // LookAt: "axis" (default +Z); TwoBone / Foot: "pole" (default none)
    f32 limit_deg = 60.0f;                 // LookAt
    f32 reach = 0.5f;                      // Foot
};

struct AnimGraphDocument {
    String name;
    Array<String> params;  // up to asset::kAnimGraphMaxParams
    u32 start = 0;         // state index
    Array<AnimGraphDocState> states;
    Array<AnimGraphDocTransition> transitions;
    String root_motion;               // the root motion bone (empty: none)
    Array<AnimGraphDocLayer> layers;  // up to asset::kAnimGraphMaxLayers
    Array<AnimGraphDocIK> ik;         // up to asset::kAnimGraphMaxIK

    // Uses a step-5 extra (2D blends, layers, IK, root motion): written as version 2 and cooked
    // with the CHNK_ANIMG extension.
    bool extended() const;
};

const char* anim_ik_kind_name(asset::AnimIKKind k);  // "two_bone", "look_at", "foot"

// Parses a .mcanim root into `out`. False when any error diagnostic was added.
bool read_animgraph_document(JsonValue doc, StringView file, AnimGraphDocument& out, DiagList& diags);

// Appends `d` as .mcanim source text (two-space indented, a trailing newline).
void write_animgraph_document(const AnimGraphDocument& d, asset::Guid guid, String& out);

// The CHNK_ANIMG payload of a document that read cleanly.
void build_animgraph_chunk(const AnimGraphDocument& d, Array<byte>& payload);

}  // namespace mce::cook
