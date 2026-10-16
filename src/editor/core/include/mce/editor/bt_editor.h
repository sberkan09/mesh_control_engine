#pragma once
// Behavior tree authoring in the editor (ROADMAP M-AI step 5): what the Behavior Tree panel drives.
//
//   BtEditor     one open .mcbt: its blackboard and its node tree, the selected node, edits and
//                undo. Every edit is written canonically through the cook's own writer
//                (cook::write_behavior_tree_document), read back and cooked (cook::build_behavior_tree)
//                before it is kept, so an edit that breaks the tree is refused with the cook's
//                message. A tree that does not cook when it is opened (a hand edit) can still be
//                edited until it does; problems() lists what the cook says about it.
//   The debugger  in play-in-editor, the brains running the open tree: each node's status (the
//                running path and how the others last finished), a utility node's scores, the root's
//                result and the blackboard's values (bt_live). bt_push_to_play runs the unsaved tree
//                in the play world without saving it.
// Headless: the editor and the debugger are tested in CI; the panel draws them.

#include "mce/cook/bt_text.h"
#include "mce/cook/diagnostics.h"
#include "mce/cook/string.h"
#include "mce/editor/editor_session.h"
#include "mce/kit/ai.h"

namespace mce::editor {

inline constexpr u32 kNoBtNode = cook::kBtNoNode;

class BtEditor {
public:
    static constexpr usize kMaxUndo = 128;

    // Opens the tree `guid` of the session's project (re-reading it when it is already open and
    // nothing is unsaved). False with last_error() set when it is not a .mcbt or does not read.
    bool open(EditorSession& session, asset::Guid guid);
    void close();
    bool is_open() const { return !guid_.is_null(); }
    asset::Guid guid() const { return guid_; }
    StringView path() const { return rel_.view(); }  // under assets/
    const cook::BtDocument& doc() const { return doc_; }
    u64 revision() const { return revision_; }  // bumped by every change of doc()

    // Creates a tree at `rel_path` (a .mcbt under assets/) with a fresh GUID: a selector over a
    // one-second wait. Cooks and opens it; refuses an existing file.
    Result<asset::Guid> create(EditorSession& session, StringView rel_path);

    // Re-reads the file when it changed on disk (an MCP tool, a text editor) and nothing is unsaved
    // here. True when it did.
    bool sync(EditorSession& session);

    // ---- selection ----
    u32 selected() const { return selected_; }
    void select(u32 node);

    // ---- edits ----
    // Each keeps doc() only when the result cooks (or, while the tree does not cook, reads back);
    // false with last_error() set otherwise (nothing changes). Edits mark the tree unsaved; a run
    // of edits until the next save() is one undo step. Node indices are the document's pre-order,
    // so an edit can renumber nodes: the node it adds or changes is selected afterwards.
    // `keep` is the node of `next` to select (kNoBtNode: none).
    bool apply(const cook::BtDocument& next, u32 keep = kNoBtNode);
    // A new node of `kind` under `parent` at child position `at` (past the end: last), selected.
    // Composites and decorators get a placeholder child ({"wait": 0}; a score under a utility),
    // leaves a value that cooks (a check or set on the first key, a log task). Refused under a leaf
    // or a decorator that has its child.
    bool add_node(u32 parent, StringView kind, u32 at = ~0u);
    // Removes `node` and its subtree. The root is not removed; a composite keeps one child.
    bool remove_node(u32 node);
    // Moves `node` `by` places among its siblings (clamped).
    bool move_node(u32 node, i32 by);
    // Puts `node` under a new decorator or composite of `kind` in its place (selected).
    bool wrap_node(u32 node, StringView kind);
    // Replaces `node` by its only child (a decorator or a one-child composite).
    bool unwrap_node(u32 node);
    // A leaf's own value as JSON ("\"chase\"", "2").
    bool set_value(u32 node, StringView json);
    // A member as JSON; empty removes it.
    bool set_field(u32 node, StringView name, StringView json);
    // Blackboard keys. Removing a key a node names is refused by the cook.
    bool add_key(StringView name, StringView type, StringView def = {});
    bool set_key(u32 key, const cook::BtDocKey& k);
    bool remove_key(u32 key);
    bool set_name(StringView name);

    // ---- the cook ----
    bool cooks() const { return cooks_; }
    // The cook's errors for doc(), "line:col: message" (empty when it cooks).
    Span<const cook::String> problems() const { return problems_.span(); }
    // The CHNK_BT payload of doc() (empty when it does not cook).
    Span<const byte> chunk() const { return chunk_.span(); }
    // The cooked node index of document node `node` (pre-order), and back. kNoBtNode when none.
    u32 cooked_index(u32 node) const;
    u32 node_at(u32 cooked) const;

    bool dirty() const { return dirty_; }
    // Writes the canonical text and hot-reloads it when there are unsaved edits.
    Result<void> save(EditorSession& session);
    bool undo(EditorSession& session);
    bool redo(EditorSession& session);
    bool can_undo() const { return !undo_.empty() || dirty_; }
    bool can_redo() const { return !redo_.empty(); }

    StringView last_error() const { return error_.view(); }
    cook::String text() const;  // the canonical .mcbt text of doc()

    // The scene entities whose AiBrain runs this tree (document order).
    void users(const EditorScene& scene, Array<EntityId>& out) const;

private:
    // Writes `next`, reads it back into `out` and cooks it; false with error_ set when it may not be kept.
    bool check(const cook::BtDocument& next, cook::BtDocument& out, cook::String& text, Array<byte>& payload,
               Array<cook::String>& problems);
    void set_doc(const cook::BtDocument& d, Array<byte>&& payload, Array<cook::String>&& problems);
    bool read_text(StringView text, cook::BtDocument& out);
    bool child_ok(u32 node) const { return node < doc_.nodes.size(); }

    asset::Guid guid_;
    cook::String rel_;
    cook::String disk_;
    cook::BtDocument doc_;
    Array<u32> order_;  // doc_.preorder
    Array<byte> chunk_;
    Array<cook::String> problems_;
    bool cooks_ = false;
    u32 selected_ = kNoBtNode;
    u64 revision_ = 0;
    bool dirty_ = false;
    cook::String error_;
    Array<cook::String> undo_, redo_;  // canonical texts
};

// ---- the debugger (play-in-editor) ----

struct BtLiveKey {
    cook::String name;
    cook::String type;
    cook::String value;  // as text: "true", "3.5", "(1, 0, 2)", "#12 Thief"; empty when not set
    bool set = false;
};

struct BtLive {
    Entity brain;
    // The play world runs a tree with this document's nodes (false after a node edit that is not
    // saved or pushed: statuses are then left Idle).
    bool current = false;
    bool active = false;
    kit::BtStatus result = kit::BtStatus::Idle;
    u32 runs = 0;
    f32 time = 0.0f;
    Array<kit::BtStatus> status;  // per document node
    Array<f32> score;             // per document node: a utility child's score; NaN otherwise
    Array<BtLiveKey> keys;        // the document's keys, in order
};

// The play-world entities whose AiBrain runs `ed`'s tree, in ECS order (empty when not playing).
void bt_play_brains(EditorSession& session, const BtEditor& ed, Array<Entity>& out);
// The state of `brain` running `ed`'s tree. False when not playing or `brain` has no such brain.
bool bt_live(EditorSession& session, const BtEditor& ed, Entity brain, BtLive& out);
// Runs `ed`'s tree as it is now (unsaved edits included) in the play world: its brains restart
// with it. False when not playing or the tree does not cook.
bool bt_push_to_play(EditorSession& session, const BtEditor& ed);

}  // namespace mce::editor
