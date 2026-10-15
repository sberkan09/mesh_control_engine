#pragma once
// Timeline authoring in the editor (ROADMAP M-CINEMA step 2, FR-EDT-130): what the Timeline panel
// drives.
//
//   TimelineEditor   one open .mctimeline: its tracks and keys, the selected track and key, the
//                    playhead, edits and undo. Every edit is written canonically through the cook's
//                    own writer (cook::write_timeline_document) and read back whole before it is
//                    kept, so a bad edit is refused with the cook's message and the file the panel
//                    saves is the one the cook reads and MCP writes.
//   Record mode      while recording, an inspector or gizmo edit of an entity the open timeline
//                    drives (the scene's TimelinePlayer entity or one of its bindings) is keyed at
//                    the playhead: a LocalTransform edit keys the entity's transform track, a float,
//                    vector, color or quaternion field keys a property track "Component.field". An
//                    entity the player does not bind yet takes the first free binding. The edit
//                    still changes the scene, like any other edit.
//   Preview          the scene played in a paused play session (the document is never touched),
//                    with the open timeline's unsaved state registered ahead of the cooked one and
//                    the player scrubbed to the playhead (kit::timeline_scrub): the viewport shows
//                    the timeline at that time, and resuming play plays it on from there.
// Headless: the editor, record mode and the preview are tested in CI; the panel draws them.

#include "mce/cook/diagnostics.h"
#include "mce/cook/string.h"
#include "mce/cook/timeline_text.h"
#include "mce/editor/editor_session.h"

namespace mce::editor {

inline constexpr u32 kNoTrack = ~0u;
inline constexpr u32 kNoKey = ~0u;

// The value a Transform / Property track takes at `t` (held before the first key and after the
// last; quaternion tracks slerp). `out` gets value_count floats (a transform track: `channel`'s).
void sample_timeline_track(const cook::TimelineDocTrack& track, f32 t, f32 out[4], u8 channel = cook::TimelineChannel::Position);

// `t` rounded to the nearest multiple of `step` (step <= 0: unchanged), never below 0.
f32 snap_time(f32 t, f32 step);

class TimelineEditor {
public:
    static constexpr usize kMaxUndo = 128;
    static constexpr f32 kSameTime = 1e-4f;  // keys closer than this sit at the same time

    // Opens the timeline `guid` of the session's project (re-reading it when it is already open and
    // nothing is unsaved). False with last_error() set when it is not a .mctimeline or does not read.
    bool open(EditorSession& session, asset::Guid guid);
    void close();
    bool is_open() const { return !guid_.is_null(); }
    asset::Guid guid() const { return guid_; }
    StringView path() const { return rel_.view(); }  // under assets/
    const cook::TimelineDocument& doc() const { return doc_; }
    // Bumped by every change of doc(): the preview re-registers the timeline when it moves.
    u64 revision() const { return revision_; }

    // Creates a timeline at `rel_path` (a .mctimeline under assets/) with a fresh GUID: `duration`
    // seconds and one event track with a "start" key at 0. Cooks and opens it; refuses an existing file.
    Result<asset::Guid> create(EditorSession& session, StringView rel_path, f32 duration = 5.0f);

    // Re-reads the file when it changed on disk (an MCP tool, a text editor) and nothing is unsaved
    // here. True when it did.
    bool sync(EditorSession& session);

    // ---- selection and playhead ----
    u32 selected_track() const { return track_; }
    u32 selected_key() const { return key_; }
    void select(u32 track, u32 key = kNoKey);
    f32 playhead() const { return playhead_; }
    void set_playhead(f32 t);  // clamped to [0, length]

    // ---- edits ----
    // Each keeps doc() only when the result reads back through the cook; false with last_error()
    // set otherwise (nothing changes). Edits mark the timeline unsaved; a run of edits until the
    // next save() is one undo step. Keys stay in time order: an edit that moves a key re-sorts its
    // track and keeps it selected.
    bool apply(const cook::TimelineDocument& next);
    // A new track (selected). Property / Activation tracks need `path` ("Component.field").
    bool add_track(cook::TimelineDocTrackType type, u8 target = asset::kTimelineSelf, StringView path = {});
    bool remove_track(u32 track);
    // A key at `t` on `track` (selected), or the key already at that time replaced by `key`.
    // `key.t` is ignored.
    bool set_key_at(u32 track, f32 t, const cook::TimelineDocKey& key);
    // A key at `t` holding the track's current value there (Transform: every channel it keys;
    // Activation: a one-second range; cue tracks: their defaults). Selected.
    bool add_key(u32 track, f32 t);
    bool remove_key(u32 track, u32 key);
    // Replaces key `key` (its time may change; the track is re-sorted and the key stays selected).
    bool set_key(u32 track, u32 key, const cook::TimelineDocKey& value);
    bool move_key(u32 track, u32 key, f32 t);
    bool set_duration(f32 seconds);  // 0: the end of the last key
    bool set_name(StringView name);

    bool dirty() const { return dirty_; }
    // Writes the canonical text and hot-reloads it when there are unsaved edits.
    Result<void> save(EditorSession& session);
    bool undo(EditorSession& session);
    bool redo(EditorSession& session);
    bool can_undo() const { return !undo_.empty() || dirty_; }
    bool can_redo() const { return !redo_.empty(); }

    StringView last_error() const { return error_.view(); }
    // The canonical .mctimeline text of doc().
    cook::String text() const;
    // The CHNK_TIMELINE payload of doc() (what the cook would write for it).
    void chunk(Array<byte>& out) const { cook::build_timeline_chunk(doc_, out); }

    // ---- the scene side ----
    // The scene entity whose TimelinePlayer plays this timeline (the first in file order), or
    // kNoEntity.
    EntityId player_entity(const EditorScene& scene) const;
    // The track target that names scene entity `id` for the player `player`: kTimelineSelf, a
    // binding index, or kNoBinding when the player does not bind it.
    static constexpr u8 kNoBinding = 0xFE;
    static u8 target_of(const EditorScene& scene, EntityId player, EntityId id);
    // The scene entity a track target names (kNoEntity when unbound).
    static EntityId entity_of(const EditorScene& scene, EntityId player, u8 target);

    // ---- record mode ----
    bool recording() const { return recording_; }
    void set_recording(bool on) { recording_ = on; }
    // EditorScene's edit observer while recording: keys the edit at the playhead (see above). Edits
    // of the TimelinePlayer itself and of entities when no scene entity plays this timeline are
    // ignored. True when a key was written.
    bool record(EditorScene& scene, EntityId id, const refl::TypeRecord& type, const void* before, const void* after);

private:
    bool read_text(StringView text, cook::TimelineDocument& out);
    void set_doc(const cook::TimelineDocument& d);
    u32 find_track(cook::TimelineDocTrackType type, u8 target, StringView path) const;
    // Inserts or replaces (same time) `key` in `track` of `next`; returns its index.
    static u32 put_key(cook::TimelineDocTrack& track, const cook::TimelineDocKey& key);

    asset::Guid guid_;
    cook::String rel_;
    cook::String disk_;  // the file as last read or written
    cook::TimelineDocument doc_;
    u32 track_ = kNoTrack, key_ = kNoKey;
    f32 playhead_ = 0.0f;
    u64 revision_ = 0;
    bool dirty_ = false;
    bool recording_ = false;
    bool in_record_ = false;  // record() is binding an entity (its own scene edit is not keyed)
    cook::String error_;
    Array<cook::String> undo_, redo_;  // canonical texts
};

// The timeline preview: the open scene in a paused play session with `ed`'s timeline scrubbed to
// its playhead. The play session is the session's own (the viewport draws it while it runs).
class TimelinePreview {
public:
    // Enters play (paused) when it is not running. False with why() set when play does not start or
    // no entity of the scene plays the timeline.
    bool start(EditorSession& session, const TimelineEditor& ed);
    // Re-registers the timeline when it changed and scrubs the player to the playhead. Call every
    // frame while previewing; a no-op when nothing moved. False when the preview is not running.
    bool update(EditorSession& session, const TimelineEditor& ed);
    void stop(EditorSession& session);
    bool running(const EditorSession& session) const;
    StringView why() const { return why_.view(); }
    // The play-world entity scrubbed (null when not running).
    Entity player() const { return player_; }

private:
    Entity player_;
    u64 revision_ = ~u64(0);
    f32 playhead_ = -1.0f;
    u64 play_ticks_ = ~u64(0);
    cook::String why_;
};

}  // namespace mce::editor
