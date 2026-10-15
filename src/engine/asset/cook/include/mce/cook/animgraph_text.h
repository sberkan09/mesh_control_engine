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

inline constexpr u32 kAnimGraphFormatVersion = 1;
inline constexpr u32 kAnimGraphAny = asset::kAnimAnyState;  // a transition's `from`: any state

const char* anim_compare_name(asset::AnimCompare op);  // ">", "<", ...

struct AnimGraphDocPoint {
    String clip;
    f32 at = 0.0f;
};

struct AnimGraphDocEvent {
    f32 time = 0.0f;
    String name;
};

struct AnimGraphDocState {
    String name;
    String clip;                       // a one-clip state (empty: a blend state)
    u8 blend_param = 0;                // a blend state: parameter index
    Array<AnimGraphDocPoint> points;   // a blend state: >= 2, increasing `at`
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

struct AnimGraphDocument {
    String name;
    Array<String> params;  // up to asset::kAnimGraphMaxParams
    u32 start = 0;         // state index
    Array<AnimGraphDocState> states;
    Array<AnimGraphDocTransition> transitions;
};

// Parses a .mcanim root into `out`. False when any error diagnostic was added.
bool read_animgraph_document(JsonValue doc, StringView file, AnimGraphDocument& out, DiagList& diags);

// Appends `d` as .mcanim source text (two-space indented, a trailing newline).
void write_animgraph_document(const AnimGraphDocument& d, asset::Guid guid, String& out);

// The CHNK_ANIMG payload of a document that read cleanly.
void build_animgraph_chunk(const AnimGraphDocument& d, Array<byte>& payload);

}  // namespace mce::cook
