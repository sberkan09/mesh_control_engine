#pragma once
// 2D skeleton authoring in the editor (ROADMAP M-ANIM2D step 5): what the Skeleton panel drives and
// what the MCP skel2d_* tools write through.
//
//   skel2d::*          document edits on the .mcskel2d text: new_text, add_bone, set_bone_rest,
//                      add_slot, add_ik, key. Each takes the text and returns the edited text in
//                      the canonical (indented) form. They only change the JSON; the cook is the
//                      validator, so a bad edit is refused by the importer with its file:line
//                      message (the editor and the tools both run it before keeping a result).
//   Skeleton2DInfo     the bones, slots, animations and constraints of a text, with the rest
//                      poses as written (degrees), for lists and fields.
//   skeleton2d_pose    a pose preview: the rest pose, or a named animation at a time, optionally
//                      with IK targets. Pure: it calls the runtime's Skeleton2DBlend and
//                      skeleton2d_solve_pose, so the numbers are the ones the tick computes.
//   Skeleton2DEditor   one open .mcskel2d: selection, an animation and a time, edits with undo,
//                      the cooked definition kept current for the preview, save and hot reload.
// Headless: the document, its edits and the preview are tested in CI; the panel draws them.

#include "mce/cook/cook.h"
#include "mce/cook/diagnostics.h"
#include "mce/cook/string.h"
#include "mce/editor/editor_session.h"
#include "mce/editor/preview_renderer.h"
#include "mce/kit/skeleton2d.h"

namespace mce::editor {

namespace skel2d {

struct BoneSpec {
    cook::String name;
    cook::String parent;  // empty: a root
    f64 x = 0, y = 0;
    f64 rotation = 0;  // degrees
    f64 scale_x = 1, scale_y = 1;
    f64 length = 0;
};

struct SlotSpec {
    cook::String name;
    cook::String bone;
    cook::String texture;  // "guid:<uuid>"; empty: a solid quad
    f64 rect[4] = {0, 0, 1, 1};
    f64 size[2] = {1, 1};
    f64 pivot[2] = {0.5, 0.5};
    f64 offset[2] = {0, 0};
    f64 rotation = 0;  // degrees
    f64 tint[4] = {1, 1, 1, 1};
    // Attachments (quads and meshes) as a JSON array in the .mcskel2d form. With them the slot
    // takes none of the quad fields above, only its tint and the attachment shown first.
    cook::String attachments_json;
    cook::String attachment;       // the one shown first (empty: the first listed)
    bool attachment_null = false;  // hide the slot at first
};

struct IkSpec {
    cook::String name;
    Array<cook::String> bones;  // the chain, parent first
    cook::String target;        // a bone outside the chain; empty: an IKTarget2D entity drives it
    f64 mix = 1;
    cook::String type;   // "two_bone" or "ccd"; empty: by chain length
    cook::String bend;   // "positive" / "negative" (two-bone); empty: the default
    u32 iterations = 0;  // CCD; 0: the default
    i32 order = 0;
};

enum class Channel : u8 { Rotate, Translate, Scale, SlotColor, SlotAttachment, IkMix };

struct KeySpec {
    cook::String animation;  // created when missing
    Channel channel = Channel::Rotate;
    cook::String target;  // the bone, slot or IK constraint
    f64 t = 0;
    f64 value = 0;     // Rotate: degrees; IkMix: the mix
    f64 x = 0, y = 0;  // Translate: metres; Scale: factors
    f64 color[4] = {1, 1, 1, 1};
    cook::String attachment;       // SlotAttachment: the attachment shown
    bool attachment_null = false;  // SlotAttachment: hide the slot
    cook::String curve;            // "", "linear" or "stepped"
    bool bezier = false;           // a cubic bezier curve instead
    f64 bezier_c[4] = {0, 0, 1, 1};
    bool has_duration = false;
    f64 duration = 0;
    bool has_loop = false;
    bool loop = true;
};

// A new document: a fresh GUID, `name` (at most 31 characters) and one root bone.
cook::String new_text(asset::Guid guid, StringView name, StringView root_bone);

// Each replaces `text` with the edited document. False with `err` set when the text is not a
// skeleton document or the edit cannot be made (a missing bone to rest-pose, a bad argument).
bool add_bone(cook::String& text, const BoneSpec& bone, cook::String& err);
// Sets a bone's rest pose (all five fields; degrees).
bool set_bone_rest(cook::String& text, StringView bone, f64 x, f64 y, f64 rotation, f64 scale_x, f64 scale_y,
                   cook::String& err);
bool add_slot(cook::String& text, const SlotSpec& slot, cook::String& err);
bool add_ik(cook::String& text, const IkSpec& ik, cook::String& err);
// Adds (or, at the same time, replaces) one key; creates the animation and the channel's list.
bool key(cook::String& text, const KeySpec& key, cook::String& err);

}  // namespace skel2d

// What a document lists, with the rest poses as written.
struct Skeleton2DInfo {
    struct Bone {
        cook::String name;
        i32 parent = -1;  // an index, -1 for a root
        f64 x = 0, y = 0, rotation = 0, scale_x = 1, scale_y = 1, length = 0;
    };
    struct Slot {
        cook::String name;
        cook::String bone;
        u32 attachments = 0;
    };
    struct Animation {
        cook::String name;
        f64 duration = 0;  // as written (0: the last key)
        bool loop = true;
    };
    Array<Bone> bones;
    Array<Slot> slots;
    Array<Animation> animations;
    Array<cook::String> ik;
    cook::String title;  // the document's "name"

    // False (with `err`) when `text` is not JSON or lacks the bone list. Does not validate.
    static bool read(StringView text, Skeleton2DInfo& out, cook::String& err);
    i32 find_bone(StringView name) const;
    i32 find_animation(StringView name) const;
};

// An IK target for a preview: where constraint `constraint` aims, in the skeleton's space.
struct Skeleton2DIKTarget {
    cook::String constraint;
    math::vec2 position;
    f32 mix = 1.0f;
};

// Cooks `text` as the asset at `rel` (in memory, against `registry`, a scan of the project) and
// defines it in `defs` under `guid`. False with the importer's diagnostics in `diags` otherwise.
bool cook_skeleton2d(const EditorProject& project, const cook::AssetRegistry& registry, asset::Guid guid,
                     StringView rel, StringView text, kit::Skeleton2DRegistry& defs, cook::DiagList& diags);

// The posed skeleton: `animation` (empty: the rest pose) sampled at `time` seconds with the keyed
// channels and IK mixes written as the animator writes them, then skeleton2d_solve_pose with the
// given IK targets. `world` receives one transform per bone, in the skeleton's space. False with
// `err` for an unknown animation or constraint.
bool skeleton2d_pose(const kit::Skeleton2DRegistry::Def& def, StringView animation, f32 time,
                     Span<const Skeleton2DIKTarget> targets, Array<kit::Bone2DTransform>& world, cook::String& err);

// The bones as overlay lines: each bone from its origin to its tip (origin + length along its
// +x axis; a short stub for a length of 0), `entity` placing the skeleton in the world. The
// selected bone (-1: none) is drawn brighter.
void skeleton2d_bone_lines(const kit::Skeleton2DRegistry::Def& def, Span<const kit::Bone2DTransform> world,
                           const kit::Bone2DTransform& entity, i32 selected, Array<OverlayLine>& out);

inline constexpr u32 kNoSkeleton2DBone = ~0u;

class Skeleton2DEditor;
class EditorScene;

// The viewport overlay for scene entity `id`: when it has a Skeleton2D playing the open skeleton,
// its bones in the editor's previewed pose (the animation and time chosen in the panel) placed by the
// entity's transform and flips. False when `id` is not such an entity.
bool skeleton2d_entity_lines(Skeleton2DEditor& ed, const EditorScene& scene, EntityId id, Array<OverlayLine>& out);

class Skeleton2DEditor {
public:
    static constexpr usize kMaxUndo = 128;

    // Opens the skeleton `guid` of the session's project (re-reading it when it is already open
    // and nothing is unsaved). False with last_error() set when it is not a .mcskel2d or does not cook.
    bool open(EditorSession& session, asset::Guid guid);
    void close();
    bool is_open() const { return !guid_.is_null(); }
    asset::Guid guid() const { return guid_; }
    StringView path() const { return rel_.view(); }  // under assets/
    const Skeleton2DInfo& info() const { return info_; }
    // Bumped by every change of the document.
    u64 revision() const { return revision_; }

    // Creates a skeleton at `rel_path` (a .mcskel2d under assets/) with a fresh GUID and a root
    // bone, cooks and opens it. Refuses an existing file.
    Result<asset::Guid> create(EditorSession& session, StringView rel_path, StringView root_bone = "root");

    // Re-reads the file when it changed on disk (an MCP tool, a text editor) and nothing is unsaved
    // here. True when it did.
    bool sync(EditorSession& session);

    // ---- selection, animation, time ----
    u32 selected_bone() const { return bone_; }
    void select_bone(u32 bone) { bone_ = bone < info_.bones.size() ? bone : kNoSkeleton2DBone; }
    // The animation previewed (empty: the rest pose) and the time in it.
    StringView animation() const { return animation_.view(); }
    void set_animation(StringView name);
    f32 time() const { return time_; }
    void set_time(f32 t) { time_ = t < 0.0f ? 0.0f : t; }
    // The previewed animation's length (0 without one).
    f32 animation_length();
    Array<Skeleton2DIKTarget>& ik_targets() { return targets_; }

    // ---- edits ----
    // Each keeps the document only when it cooks; false with last_error() set otherwise (nothing
    // changes). Edits mark the document unsaved; a run of edits until the next save() is one undo step.
    bool apply_text(StringView next);
    bool add_bone(const skel2d::BoneSpec& bone);
    bool set_bone_rest(u32 bone, f64 x, f64 y, f64 rotation, f64 scale_x, f64 scale_y);
    bool add_slot(const skel2d::SlotSpec& slot);
    bool add_ik(const skel2d::IkSpec& ik);
    bool key(const skel2d::KeySpec& key);

    bool dirty() const { return dirty_; }
    // Writes the text and hot-reloads it when there are unsaved edits.
    Result<void> save(EditorSession& session);
    bool undo(EditorSession& session);
    bool redo(EditorSession& session);
    bool can_undo() const { return !undo_.empty() || dirty_; }
    bool can_redo() const { return !redo_.empty(); }

    StringView last_error() const { return error_.view(); }
    StringView text() const { return text_.view(); }

    // ---- the preview ----
    // The world transform of every bone for animation() at time() (rest: no animation), IK
    // targets included. Empty when the document does not cook. Recomputed on every call (a pose is
    // a few dozen transforms).
    Span<const kit::Bone2DTransform> pose();
    const kit::Skeleton2DRegistry::Def* def();

private:
    bool take(StringView next);
    bool load(StringView text);

    asset::Guid guid_;
    cook::String rel_;
    cook::String text_;
    cook::String disk_;  // the file as last read or written
    Skeleton2DInfo info_;
    kit::Skeleton2DRegistry defs_;
    cook::AssetRegistry registry_;
    const EditorProject* project_ = nullptr;
    u32 bone_ = kNoSkeleton2DBone;
    cook::String animation_;
    f32 time_ = 0.0f;
    Array<Skeleton2DIKTarget> targets_;
    Array<kit::Bone2DTransform> pose_;
    u64 revision_ = 0;
    bool dirty_ = false;
    cook::String error_;
    Array<cook::String> undo_, redo_;  // texts
};

}  // namespace mce::editor
