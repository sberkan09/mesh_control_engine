// The Timeline panel's editor (ROADMAP M-CINEMA step 2, FR-EDT-130), headless: the canonical text
// round-trips through the cook, edits are refused with the cook's message, undo and redo land on
// disk, record mode keys inspector edits at the playhead (binding new entities), and the preview
// scrubs the play world without touching the document.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>

#include "cook_test_util.h"
#include "mce/cook/fs.h"
#include "mce/cook/json.h"
#include "mce/cook/timeline_text.h"
#include "mce/ecs/transform.h"
#include "mce/editor/editor_session.h"
#include "mce/editor/timeline_editor.h"
#include "mce/kit/timeline.h"
#include "mce/kit/kit3d.h"

using namespace mce;
using namespace mce::editor;
using namespace cook_test;
using cook::TimelineDocTrackType;

namespace {

StringView sv(const std::string& s) { return StringView(s.data(), s.size()); }

const char* kIntro = R"({ "format": "mce-timeline", "version": 1,
    "guid": "7e200000-0000-4000-8000-000000000001", "name": "intro", "duration": 2,
    "tracks": [
      { "type": "transform", "target": 0, "keys": [
          { "t": 0, "position": [0, 0, 0] },
          { "t": 1, "position": [4, 0, 0], "ease": "smooth" },
          { "t": 2, "position": [4, 2, 0], "rotation": [0, 0.7071068, 0, 0.7071068] } ] },
      { "type": "property", "target": 1, "path": "Camera3DComponent.fov_deg", "keys": [
          { "t": 0, "value": 60 }, { "t": 2, "value": 90 } ] },
      { "type": "activation", "target": 0, "path": "MeshRef.visible", "ranges": [ { "from": 0.5, "to": 1.5 } ] },
      { "type": "event", "keys": [ { "t": 1, "emit": "beat" } ] },
      { "type": "subtitle", "lines": [ { "t": 0.25, "duration": 1, "text": "Hello", "speaker": "Mira" } ] }
    ] })";

std::string timeline_project(const char* name) {
    const std::string r = fresh_dir(name);
    write_file(r + "/assets/intro.mctimeline", kIntro);
    write_file(r + "/assets/stage.mcscene", R"({ "format": "mce-scene", "version": 1,
        "guid": "7e200000-0000-4000-8000-000000000003", "name": "stage", "entities": [
        { "name": "Director", "components": {
            "TimelinePlayer": { "timeline": "guid:7e200000-0000-4000-8000-000000000001",
                                "bindings": ["$1", "$2", null, null, null, null, null, null], "playing": false } } },
        { "name": "Door", "components": { "LocalTransform": { "position": [0, 0, 0] }, "MeshRef": { "visible": 0 } } },
        { "name": "Cam", "components": { "LocalTransform": {}, "Camera3DComponent": { "fov_deg": 45, "primary": 1 } } },
        { "name": "Lamp", "components": { "LocalTransform": { "position": [1, 1, 1] } } } ] })");
    return r;
}

const asset::Guid kIntroGuid = asset::parse_guid("7e200000-0000-4000-8000-000000000001").value();

std::string text_of(const cook::String& s) { return std::string(s.view().data(), s.view().size()); }

std::string file_text(const std::string& path) {
    cook::String s;
    REQUIRE(cook::fs::read_text(sv(path), s));
    return text_of(s);
}

std::string serialized(const EditorScene& s) {
    cook::String t;
    s.serialize(t);
    return text_of(t);
}

EntityId entity_named(const EditorScene& s, const char* name) {
    for (const DocEntity& e : s.doc().entities())
        if (e.name.view() == StringView(name)) return e.id;
    return kNoEntity;
}

template <typename C>
C component_of(const EditorScene& s, EntityId id) {
    const DocComponent* c = s.doc().find(id)->find(refl::type_of<C>());
    REQUIRE(c != nullptr);
    C v;
    std::memcpy(&v, c->data(), sizeof(C));
    return v;
}

void open_stage(EditorSession& session, const std::string& root) {
    REQUIRE(session.open_project(sv(root)));
    REQUIRE(session.open_scene("stage.mcscene"));
}

}  // namespace

TEST_CASE("timeline editor: the canonical text reads back to the same document and chunk") {
    const std::string root = timeline_project("tl_ed_roundtrip");
    EditorSession session;
    open_stage(session, root);
    TimelineEditor ed;
    REQUIRE(ed.open(session, kIntroGuid));
    CHECK(ed.doc().tracks.size() == 5);
    CHECK(ed.doc().length() == doctest::Approx(2.0f));

    // Writing what was read and reading it again gives the same text and the same cooked bytes.
    const std::string a = text_of(ed.text());
    Array<byte> chunk_a;
    ed.chunk(chunk_a);
    cook::JsonDoc doc;
    cook::JsonError err;
    REQUIRE(doc.parse(sv(a), err));
    cook::TimelineDocument back;
    cook::DiagList diags;
    REQUIRE(cook::read_timeline_document(doc.root(), "intro.mctimeline", back, diags));
    cook::String b;
    cook::write_timeline_document(back, kIntroGuid, b);
    CHECK(text_of(b) == a);
    Array<byte> chunk_b;
    cook::build_timeline_chunk(back, chunk_b);
    REQUIRE(chunk_a.size() == chunk_b.size());
    CHECK(std::memcmp(chunk_a.data(), chunk_b.data(), chunk_a.size()) == 0);
    REQUIRE(asset::TimelineView::read(chunk_a.span()));

    // Sampling: the smooth-eased position halfway between 1 and 2 s is held from the key at 1 s.
    f32 v[4];
    sample_timeline_track(ed.doc().tracks[0], 0.5f, v);
    CHECK(v[0] == doctest::Approx(2.0f));
    sample_timeline_track(ed.doc().tracks[0], 1.5f, v);
    CHECK(v[0] == doctest::Approx(4.0f));
    CHECK(v[1] == doctest::Approx(1.0f));
    sample_timeline_track(ed.doc().tracks[1], 1.0f, v);
    CHECK(v[0] == doctest::Approx(75.0f));
    CHECK(snap_time(0.26f, 0.1f) == doctest::Approx(0.3f));
    CHECK(snap_time(-1.0f, 0.1f) == 0.0f);
    CHECK(snap_time(0.26f, 0.0f) == doctest::Approx(0.26f));
}

TEST_CASE("timeline editor: edits, refusals, undo and redo on disk") {
    const std::string root = timeline_project("tl_ed_edits");
    EditorSession session;
    open_stage(session, root);
    TimelineEditor ed;
    REQUIRE(ed.open(session, kIntroGuid));
    const std::string file = root + "/assets/intro.mctimeline";
    const std::string original = text_of(ed.text());

    // A property track the cook refuses: the cook's message, nothing changes.
    const u64 rev = ed.revision();
    CHECK_FALSE(ed.add_track(TimelineDocTrackType::Property, 0, "LocalTransform.wobble"));
    CHECK(std::string(ed.last_error().data(), ed.last_error().size()).find("wobble") != std::string::npos);
    CHECK(ed.revision() == rev);
    CHECK_FALSE(ed.dirty());

    // Add a key at the playhead on the fov track: it holds the sampled value and is selected.
    ed.set_playhead(1.0f);
    REQUIRE(ed.add_key(1, ed.playhead()));
    CHECK(ed.doc().tracks[1].keys.size() == 3);
    CHECK(ed.selected_track() == 1);
    CHECK(ed.selected_key() == 1);
    CHECK(ed.doc().tracks[1].keys[1].value[0] == doctest::Approx(75.0f));
    CHECK(ed.dirty());

    // Move it past the last key: the track re-sorts and the key stays selected.
    REQUIRE(ed.move_key(1, 1, 1.75f));
    CHECK(ed.selected_key() == 1);
    REQUIRE(ed.move_key(1, 1, 1.9f));
    CHECK(ed.doc().tracks[1].keys[1].t == doctest::Approx(1.9f));
    // The duration cannot end before the last key.
    CHECK_FALSE(ed.set_duration(0.5f));
    REQUIRE(ed.set_duration(3.0f));
    REQUIRE(ed.add_track(TimelineDocTrackType::Event));
    CHECK(ed.doc().tracks.size() == 6);

    // Save writes the canonical text; the cook reads it.
    REQUIRE(ed.save(session));
    CHECK_FALSE(ed.dirty());
    const std::string saved = file_text(file);
    CHECK(saved == text_of(ed.text()));
    CHECK(saved != original);

    // One undo step for the run of edits before the save; it lands on disk.
    REQUIRE(ed.can_undo());
    REQUIRE(ed.undo(session));
    CHECK(file_text(file) == original);
    CHECK(ed.doc().tracks.size() == 5);
    REQUIRE(ed.can_redo());
    REQUIRE(ed.redo(session));
    CHECK(file_text(file) == saved);
    CHECK(ed.doc().tracks.size() == 6);

    // Removing a track's last key removes the track.
    REQUIRE(ed.remove_key(5, 0));
    CHECK(ed.doc().tracks.size() == 5);

    // An outside write is picked up when nothing is unsaved here.
    REQUIRE(ed.save(session));
    REQUIRE(session.write_asset("intro.mctimeline", kIntro));
    CHECK(ed.sync(session));
    CHECK(text_of(ed.text()) == original);
}

TEST_CASE("timeline editor: create makes a cooked timeline with a start event") {
    const std::string root = timeline_project("tl_ed_create");
    EditorSession session;
    open_stage(session, root);
    TimelineEditor ed;
    Result<asset::Guid> g = ed.create(session, "cut/new.mctimeline", 4.0f);
    REQUIRE(g);
    CHECK(ed.guid() == g.value());
    CHECK(ed.doc().duration == doctest::Approx(4.0f));
    REQUIRE(ed.doc().tracks.size() == 1);
    CHECK(ed.doc().tracks[0].type == TimelineDocTrackType::Event);
    CHECK(session.project().find(g.value()) != nullptr);
    CHECK_FALSE(ed.create(session, "cut/new.mctimeline"));
    CHECK_FALSE(ed.create(session, "cut/new.json"));
}

TEST_CASE("timeline editor: record mode keys inspector edits at the playhead") {
    const std::string root = timeline_project("tl_ed_record");
    EditorSession session;
    open_stage(session, root);
    EditorScene& scene = session.scene();
    TimelineEditor ed;
    REQUIRE(ed.open(session, kIntroGuid));
    scene.set_edit_observer(
        [](void* user, EditorScene& s, EntityId id, const refl::TypeRecord& type, const void* before, const void* after) {
            static_cast<TimelineEditor*>(user)->record(s, id, type, before, after);
        },
        &ed);

    const EntityId director = entity_named(scene, "Director");
    const EntityId door = entity_named(scene, "Door");
    const EntityId cam = entity_named(scene, "Cam");
    const EntityId lamp = entity_named(scene, "Lamp");
    REQUIRE(ed.player_entity(scene) == director);
    CHECK(TimelineEditor::target_of(scene, director, door) == 0);
    CHECK(TimelineEditor::target_of(scene, director, director) == asset::kTimelineSelf);
    CHECK(TimelineEditor::target_of(scene, director, lamp) == TimelineEditor::kNoBinding);
    CHECK(TimelineEditor::entity_of(scene, director, 1) == cam);

    // Not recording: an edit keys nothing.
    LocalTransform lt = component_of<LocalTransform>(scene, door);
    lt.position = {9.0f, 0.0f, 0.0f};
    REQUIRE(scene.set(door, lt));
    CHECK_FALSE(ed.dirty());

    // Recording: moving the door at 0.5 s keys its transform track (position channel only).
    ed.set_recording(true);
    ed.set_playhead(0.5f);
    lt.position = {1.0f, 2.0f, 3.0f};
    REQUIRE(scene.set(door, lt));
    REQUIRE(ed.dirty());
    const cook::TimelineDocTrack& tr = ed.doc().tracks[0];
    REQUIRE(tr.keys.size() == 4);
    CHECK(tr.keys[1].t == doctest::Approx(0.5f));
    CHECK(tr.keys[1].channels == cook::TimelineChannel::Position);
    CHECK(tr.keys[1].position[1] == doctest::Approx(2.0f));
    CHECK(ed.selected_track() == 0);
    CHECK(ed.selected_key() == 1);

    // Rotating it at the same time merges into that key.
    lt.rotation = math::normalize(math::quat{0.0f, 1.0f, 0.0f, 1.0f});
    REQUIRE(scene.set(door, lt));
    CHECK(ed.doc().tracks[0].keys.size() == 4);
    CHECK(ed.doc().tracks[0].keys[1].channels == (cook::TimelineChannel::Position | cook::TimelineChannel::Rotation));

    // A float field keys the existing property track.
    kit::Camera3DComponent c = component_of<kit::Camera3DComponent>(scene, cam);
    c.fov_deg = 70.0f;
    REQUIRE(scene.set(cam, c));
    CHECK(ed.doc().tracks[1].keys.size() == 3);
    CHECK(ed.doc().tracks[1].keys[1].value[0] == doctest::Approx(70.0f));

    // An entity the player does not bind takes the first free binding, and a new track.
    LocalTransform ll = component_of<LocalTransform>(scene, lamp);
    ll.scale = {2.0f, 2.0f, 2.0f};
    REQUIRE(scene.set(lamp, ll));
    CHECK(TimelineEditor::target_of(scene, director, lamp) == 2);
    const cook::TimelineDocTrack& nt = ed.doc().tracks[ed.selected_track()];
    CHECK(nt.type == TimelineDocTrackType::Transform);
    CHECK(nt.target == 2);
    CHECK(nt.keys[0].channels == cook::TimelineChannel::Scale);

    // Undo on the scene side takes back the binding; the timeline keeps its own history.
    REQUIRE(ed.save(session));
    REQUIRE(ed.undo(session));
    CHECK(ed.doc().tracks.size() == 5);
    scene.set_edit_observer(nullptr, nullptr);
}

TEST_CASE("timeline editor: the preview scrubs the play world and never touches the document") {
    const std::string root = timeline_project("tl_ed_preview");
    EditorSession session;
    open_stage(session, root);
    TimelineEditor ed;
    REQUIRE(ed.open(session, kIntroGuid));
    const std::string before = serialized(session.scene());

    TimelinePreview preview;
    REQUIRE(preview.start(session, ed));
    REQUIRE(preview.running(session));
    World& w = *session.play().world();

    auto door_x = [&]() {
        const kit::TimelinePlayer* p = w.try_get<kit::TimelinePlayer>(preview.player());
        REQUIRE(p != nullptr);
        const LocalTransform* t = w.try_get<LocalTransform>(p->bindings[0]);
        REQUIRE(t != nullptr);
        return t->position.x;
    };
    ed.set_playhead(0.5f);
    REQUIRE(preview.update(session, ed));
    CHECK(door_x() == doctest::Approx(2.0f));
    CHECK(w.try_get<kit::TimelinePlayer>(preview.player())->time == doctest::Approx(0.5f));

    // An unsaved edit shows in the preview at once.
    REQUIRE(ed.set_key(0, 1, [&] {
        cook::TimelineDocKey k = ed.doc().tracks[0].keys[1];
        k.position[0] = 8.0f;
        return k;
    }()));
    REQUIRE(preview.update(session, ed));
    CHECK(door_x() == doctest::Approx(4.0f));
    ed.set_playhead(1.0f);
    REQUIRE(preview.update(session, ed));
    CHECK(door_x() == doctest::Approx(8.0f));

    preview.stop(session);
    CHECK_FALSE(session.play().active());
    CHECK(serialized(session.scene()) == before);
    CHECK_FALSE(session.scene().dirty());
}
