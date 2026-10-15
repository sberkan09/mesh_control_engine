#pragma once
// Animation editing in the editor (ROADMAP M-CINEMA step 3): what the Animation panel drives.
//
//   AnimGraphEditor   one open .mcanim state graph: its parameters, states (one clip or a 1D blend
//                     of clips), transitions and where the panel draws each state. Every edit is
//                     written canonically through the cook's own writer (cook::write_animgraph_document)
//                     and read back whole before it is kept, so a bad edit is refused with the cook's
//                     message, and the file the panel saves is the one the cook reads and MCP writes.
//   Clips             the clips of a cooked mesh (names and durations), for the clip pickers.
//   AnimPreview       an entity with an Animator, played in a paused play session (the document is
//                     never touched): one clip posed at a time the panel scrubs, or the entity's graph
//                     running with parameter values the panel sets.
// Headless: the editor and the preview are tested in CI; the panel draws them.

#include "mce/cook/animgraph_text.h"
#include "mce/cook/diagnostics.h"
#include "mce/cook/string.h"
#include "mce/editor/editor_session.h"

namespace mce::editor {

inline constexpr u32 kNoState = ~0u;
inline constexpr u32 kNoTransition = ~0u;

struct MeshClipInfo {
    cook::String name;
    f32 duration = 0.0f;
};

// The clips of the cooked mesh `mesh` in the session's project, in file order. False when the mesh
// is not cooked or does not read.
bool mesh_clips(EditorSession& session, asset::Guid mesh, Array<MeshClipInfo>& out);

class AnimGraphEditor {
public:
    static constexpr usize kMaxUndo = 128;

    // Opens the graph `guid` of the session's project (re-reading it when it is already open and
    // nothing is unsaved). False with last_error() set when it is not an .mcanim or does not read.
    bool open(EditorSession& session, asset::Guid guid);
    void close();
    bool is_open() const { return !guid_.is_null(); }
    asset::Guid guid() const { return guid_; }
    StringView path() const { return rel_.view(); }  // under assets/
    const cook::AnimGraphDocument& doc() const { return doc_; }
    u64 revision() const { return revision_; }  // bumped by every change of doc()

    // Creates a graph at `rel_path` (an .mcanim under assets/) with a fresh GUID and one state
    // "idle" playing `clip`. Cooks and opens it; refuses an existing file.
    Result<asset::Guid> create(EditorSession& session, StringView rel_path, StringView clip = "Idle");

    // Re-reads the file when it changed on disk and nothing is unsaved here. True when it did.
    bool sync(EditorSession& session);

    // ---- selection ----
    u32 selected_state() const { return state_; }
    u32 selected_transition() const { return transition_; }
    void select_state(u32 i);       // clears the transition selection
    void select_transition(u32 i);  // clears the state selection

    // ---- edits ----
    // Each keeps doc() only when the result reads back through the cook; false with last_error()
    // set otherwise (nothing changes). Edits mark the graph unsaved; a run of edits until the next
    // save() is one undo step.
    bool apply(const cook::AnimGraphDocument& next);
    // A state playing `clip` at `pos` (selected). The name is made unique ("walk", "walk 2", ...).
    bool add_state(StringView name, StringView clip, f32 x, f32 y);
    // Removes the state and every transition to or from it; the start moves to the first state.
    bool remove_state(u32 i);
    bool set_state(u32 i, const cook::AnimGraphDocState& s);  // renames keep transitions pointing at it
    bool move_state(u32 i, f32 x, f32 y);
    bool set_start(u32 i);
    // A transition (selected): on the end of `from` when it does not loop, else when the first
    // parameter is > 0.5. `from` may be cook::kAnimGraphAny. Refused when there is no parameter
    // to wait on and `from` loops.
    bool add_transition(u32 from, u32 to);
    bool remove_transition(u32 i);
    bool set_transition(u32 i, const cook::AnimGraphDocTransition& t);
    // Parameters: up to four. Removing one that a blend or a condition uses is refused.
    bool add_param(StringView name);
    bool rename_param(u32 i, StringView name);
    bool remove_param(u32 i);
    bool set_name(StringView name);

    // Gives every state without a position one on a grid (not an edit: positions are saved with the
    // next edit). Call after open.
    void layout();
    // Where the panel draws state `i` (its pos, or its layout()).
    void position(u32 i, f32& x, f32& y) const;

    bool dirty() const { return dirty_; }
    Result<void> save(EditorSession& session);
    bool undo(EditorSession& session);
    bool redo(EditorSession& session);
    bool can_undo() const { return !undo_.empty() || dirty_; }
    bool can_redo() const { return !redo_.empty(); }

    StringView last_error() const { return error_.view(); }
    cook::String text() const;  // the canonical .mcanim text of doc()
    void chunk(Array<byte>& out) const { cook::build_animgraph_chunk(doc_, out); }

    // The scene entities whose Animator names this graph (document order).
    void users(const EditorScene& scene, Array<EntityId>& out) const;

private:
    bool read_text(StringView text, cook::AnimGraphDocument& out);
    void set_doc(const cook::AnimGraphDocument& d);
    cook::String unique_state_name(StringView base) const;

    asset::Guid guid_;
    cook::String rel_;
    cook::String disk_;
    cook::AnimGraphDocument doc_;
    Array<f32> layout_;  // x, y per state
    u32 state_ = kNoState, transition_ = kNoTransition;
    u64 revision_ = 0;
    bool dirty_ = false;
    cook::String error_;
    Array<cook::String> undo_, redo_;
};

// Clip and graph preview of one scene entity with an Animator (and a MeshRef for its clips).
class AnimPreview {
public:
    // Enters play (paused) when it is not running. False with why() set when play does not start
    // or `entity` has no Animator.
    bool start(EditorSession& session, EntityId entity);
    void stop(EditorSession& session);
    bool running(const EditorSession& session) const;
    StringView why() const { return why_.view(); }
    EntityId entity() const { return doc_entity_; }
    Entity player() const { return entity_; }  // the play-world entity

    // Poses `clip` at `seconds` (the graph is set aside while a clip is shown). Play stays paused.
    bool show_clip(EditorSession& session, StringView clip, f32 seconds, bool loop = true);
    // Runs the entity's own graph again with these parameter values (in the graph's order) and
    // resumes play; the Animator's state then follows the graph.
    bool run_graph(EditorSession& session, const f32 params[4]);
    // The graph state the play world is in (empty when none).
    StringView state(const EditorSession& session) const;

private:
    EntityId doc_entity_ = kNoEntity;
    Entity entity_;
    asset::AssetRef<asset::AnimGraph> graph_;  // the entity's graph, set aside by show_clip
    cook::String why_;
};

}  // namespace mce::editor
