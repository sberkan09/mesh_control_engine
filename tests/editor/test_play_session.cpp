// Play-in-editor (FR-EDT-060, NFR-M-03, ADR-0029), headless: enter builds the play world from the
// document the way mce_player builds it from the cooked pack, pause/step/slow-mo ride the fixed
// step, Play -> Stop -> Play reproduces the first-tick state, and play never touches the document.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "cook_test_util.h"
#include "mce/ecs/transform.h"
#include "mce/editor/editor_session.h"
#include "mce/kit/components.h"
#include "mce/physics/components.h"
#include "mce/platform/process.h"

using namespace mce;
using namespace mce::editor;
using namespace cook_test;

namespace {

const std::string kPlayAssets = std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets";

std::string play_project(const char* name) {
    const std::string root = fresh_dir(name);
    std::filesystem::copy(kPlayAssets, root + "/assets", std::filesystem::copy_options::recursive);
    return root;
}

std::string serialized(const EditorScene& s) {
    cook::String t;
    s.serialize(t);
    return std::string(t.view().data(), t.view().size());
}

// mce_player -headless on the cooked pack: the line "N ticks, M entities, state_hash H".
std::string player_hash(const std::string& pack, const std::string& guid, int ticks) {
    const std::string t = std::to_string(ticks);
    const StringView args[] = {MCE_PLAYER_EXE, "-headless", "1", "-ticks", StringView(t.data(), t.size()), "-pack",
                               StringView(pack.data(), pack.size()), "-scene", StringView(guid.data(), guid.size())};
    platform::Process player;
    if (!player.start(Span<const StringView>(args, 9), true)) return {};
    Array<char> out;
    if (player.wait(&out) != 0) return {};
    const std::string text(out.data(), out.size());
    const usize at = text.find("state_hash ");
    return at == std::string::npos ? std::string() : text.substr(at + 11, 16);
}

std::string hex(u64 v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

}  // namespace

TEST_CASE("play: enter builds the player's world from the document; same hash as mce_player") {
    const std::string root = play_project("play_parity");
    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    REQUIRE(session.open_scene("scenes/demo.mcscene"));
    const std::string before = serialized(session.scene());

    REQUIRE(session.enter_play());
    PlaySession& play = session.play();
    CHECK(play.state() == PlaySession::State::Playing);
    CHECK(play.entity_count() == session.scene().doc().size());
    CHECK_MESSAGE(play.enter_seconds() < 1.0, "NFR-M-03: play enter took " << play.enter_seconds() << " s");
    MESSAGE("play enter: " << play.enter_seconds() * 1000.0 << " ms, " << play.entity_count() << " entities");
    play.pause();
    for (int i = 0; i < 600; ++i) play.step();
    CHECK(play.ticks() == 600);
    const u64 hash = play.state_hash();
    play.stop();
    CHECK_FALSE(play.active());
    CHECK(play.world() == nullptr);

    // The cooked pack of the saved file plays to the same hash in mce_player.
    REQUIRE(session.cook(true).ok());
    const std::string guid(asset::to_string(session.scene().doc().guid()).c_str());
    CHECK(player_hash(root + "/assets.mce", guid, 600) == hex(hash));
    CHECK(serialized(session.scene()) == before);
    CHECK_FALSE(session.scene().dirty());
}

TEST_CASE("play: Play -> Stop -> Play gives the same first-tick state and the same run") {
    const std::string root = play_project("play_twice");
    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    REQUIRE(session.open_scene("scenes/demo.mcscene"));
    PlaySession& play = session.play();

    u64 enter[2], first[2], last[2];
    for (int run = 0; run < 2; ++run) {
        REQUIRE(session.enter_play());
        enter[run] = play.enter_hash();
        CHECK(play.state_hash() == enter[run]);
        play.pause();
        play.step();
        first[run] = play.state_hash();
        play.resume();
        while (play.ticks() < 300) play.advance(1.0 / 60.0);
        last[run] = play.state_hash();
        session.stop_play();
    }
    CHECK(enter[0] == enter[1]);
    CHECK(first[0] == first[1]);
    CHECK(last[0] == last[1]);
    CHECK(first[0] != enter[0]);  // the scene has dynamic bodies: a tick changes it
}

TEST_CASE("play: pause, single-step and slow motion ride the fixed step") {
    const std::string root = play_project("play_controls");
    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    REQUIRE(session.open_scene("scenes/demo.mcscene"));
    PlaySession& play = session.play();
    REQUIRE(session.enter_play());

    CHECK(play.advance(1.0 / 60.0) == 1);
    CHECK(play.advance(0.5) == PlaySession::kMaxCatchup);  // a long frame is clamped
    const u64 t = play.ticks();
    play.pause();
    CHECK(play.advance(1.0) == 0);
    CHECK(play.ticks() == t);
    play.step();
    CHECK(play.ticks() == t + 1);
    play.step();
    CHECK(play.ticks() == t + 2);
    play.resume();
    play.set_time_scale(0.25f);  // slow-mo: four frames per tick
    u32 n = 0;
    for (int i = 0; i < 8; ++i) n += play.advance(1.0 / 60.0);
    CHECK(n == 2);
    play.set_time_scale(2.0f);
    CHECK(play.advance(1.0 / 60.0) == 2);
    play.step();  // while playing: pauses first
    CHECK(play.state() == PlaySession::State::Paused);
}

TEST_CASE("play: unsaved edits play, play-world changes are discarded, cook and open stop play") {
    const std::string root = play_project("play_edits");
    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    REQUIRE(session.open_scene("scenes/demo.mcscene"));
    EditorScene& s = session.scene();

    const EntityId extra = s.create_entity("Extra");
    LocalTransform t;
    t.position = {3, 4, 0};
    s.set(extra, t);
    const std::string edited = serialized(s);
    REQUIRE(session.enter_play());
    PlaySession& play = session.play();
    CHECK(play.entity_count() == s.doc().size());
    const Entity e = play.entities()[static_cast<usize>(s.doc().index_of(extra))];
    REQUIRE(play.world()->try_get<LocalTransform>(e));
    CHECK(play.world()->try_get<LocalTransform>(e)->position.x == 3.0f);

    // Changing the play world never reaches the document or the preview world.
    play.world()->try_get_mut<LocalTransform>(e)->position.x = 99.0f;
    play.advance(1.0 / 60.0);
    CHECK(s.get<LocalTransform>(extra)->position.x == 3.0f);
    CHECK(s.verify_world());
    session.stop_play();
    CHECK(serialized(s) == edited);

    REQUIRE(session.enter_play());
    session.cook(false);
    CHECK_FALSE(play.active());
    REQUIRE(session.enter_play());
    session.new_scene("other");
    CHECK_FALSE(play.active());
}

TEST_CASE("play: a document that does not cook does not play, with a file:line reason") {
    const std::string root = play_project("play_broken");
    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    REQUIRE(session.open_scene("scenes/demo.mcscene"));
    EditorScene& s = session.scene();
    const EntityId id = s.create_entity("Bad");
    s.add_component(id, refl::type_of<kit::Sprite>());
    kit::Sprite sp;
    sp.texture.guid = asset::Guid{0x1234, 0x5678};  // no such asset
    s.set(id, sp);
    CHECK_FALSE(session.enter_play());
    CHECK_FALSE(session.play().active());
    CHECK(session.play_diags().error_count() > 0);
}
