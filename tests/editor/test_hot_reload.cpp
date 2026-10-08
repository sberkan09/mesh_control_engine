// Asset hot reload in the editor (FR-AST-090, ADR-0029 §2), headless: the assets/ watcher, the
// re-cook + commit_reload swap (handles stay valid, the mount is not replaced), materials, scenes
// changed on disk, a broken texture keeping the old generation, and play surviving a reload. The
// stopwatch numbers land in docs/milestones/M5b.md.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "cook_test_util.h"
#include "mce/asset/formats.h"
#include "mce/editor/editor_session.h"
#include "mce/platform/platform.h"

using namespace mce;
using namespace mce::editor;
using namespace cook_test;

namespace {

const std::string kReloadAssets = std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets";

std::string reload_project(const char* name) {
    const std::string root = fresh_dir(name);
    std::filesystem::copy(kReloadAssets, root + "/assets", std::filesystem::copy_options::recursive);
    return root;
}

void put(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

// Bumps the file's mtime past the filesystem's resolution so the watcher sees a change even when
// the size stays the same.
void touch_later(const std::string& path) {
    const auto t = std::filesystem::last_write_time(path);
    std::filesystem::last_write_time(path, t + std::chrono::seconds(2));
}

asset::AssetKey key_of(EditorSession& s, const char* rel) {
    for (const ProjectAsset& a : s.project().assets())
        if (a.path.view() == rel) return s.project().mounted()->find(a.guid);
    return asset::kInvalidAssetKey;
}

constexpr const char* kMatGuid = "6f1c2a3b-4d5e-4f60-8a7b-9c0d1e2f3a4b";

std::string material(f32 roughness) {
    return std::string("{ \"format\": \"mce-material\", \"version\": 1, \"guid\": \"") + kMatGuid +
           "\", \"base_color\": [1, 0.5, 0.25, 1], \"roughness\": " + std::to_string(roughness) + " }\n";
}

}  // namespace

TEST_CASE("hot reload: the watcher sees additions, edits and removals under assets/") {
    const std::string root = reload_project("hr_watch");
    AssetWatcher w;
    w.watch(StringView(root.data(), root.size()));
    Array<cook::String> changed;
    CHECK_FALSE(w.poll(0.0, 0.5, &changed));
    put(root + "/assets/notes.txt", "x");
    CHECK_FALSE(w.poll(0.2, 0.5, &changed));  // inside the interval: no scan
    REQUIRE(w.poll(0.6, 0.5, &changed));
    REQUIRE(changed.size() == 1);
    CHECK(changed[0].view() == "notes.txt");
    put(root + "/assets/notes.txt", "xy");
    REQUIRE(w.poll(1.2, 0.5, &changed));
    CHECK(changed[0].view() == "notes.txt");
    touch_later(root + "/assets/textures/ball.png");
    REQUIRE(w.poll(1.8, 0.5, &changed));
    CHECK(changed[0].view() == "textures/ball.png");
    std::filesystem::remove(root + "/assets/notes.txt");
    REQUIRE(w.poll(2.4, 0.5, &changed));
    CHECK(changed[0].view() == "notes.txt");
    CHECK_FALSE(w.poll(3.0, 0.5, &changed));
}

TEST_CASE("hot reload: an edited texture re-cooks and swaps into the live mount in under a second") {
    const std::string root = reload_project("hr_texture");
    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    REQUIRE(session.open_scene("scenes/demo.mcscene"));
    session.tick_hot_reload(0.0, 0.0);  // baseline scan
    asset::AssetSystem* mount = session.project().mounted();
    REQUIRE(mount);
    const asset::AssetKey crate = key_of(session, "textures/crate.png");
    REQUIRE(crate != asset::kInvalidAssetKey);
    auto handle = mount->load_sync<asset::Texture>(crate);
    REQUIRE(handle);
    const u32 gen_before = mount->generation(crate);
    const u64 gen_project = session.project().generation();

    // The stopwatch: from the file landing on disk to the new generation being resident.
    std::filesystem::copy_file(root + "/assets/textures/ball.png", root + "/assets/textures/crate.png",
                               std::filesystem::copy_options::overwrite_existing);
    touch_later(root + "/assets/textures/crate.png");
    const f64 t0 = platform::now_seconds();
    REQUIRE(session.tick_hot_reload(1.0, 0.0));
    const f64 elapsed = platform::now_seconds() - t0;
    const HotReloadResult& r = session.last_hot_reload();
    CHECK(r.reloaded >= 1);  // the texture, plus whatever cooks it into its output (a material)
    CHECK(r.failed == 0);
    CHECK_FALSE(r.remounted);
    CHECK(session.project().mounted() == mount);  // the same mount: holders keep their pointer
    CHECK(session.project().generation() == gen_project);
    CHECK(mount->generation(crate) != gen_before);
    CHECK(mount->is_alive(handle.value()) == false);  // the old handle is stale...
    asset::AssetRef<asset::Texture> ref;
    ref.guid = mount->info(crate).guid;
    ref.handle = handle.value();
    CHECK_FALSE(mount->resolve(ref).empty());  // ...and re-stamps to the live generation
    CHECK(ref.handle.generation == mount->generation(crate));
    CHECK_MESSAGE(elapsed < 1.0, "FR-AST-090: texture reload took " << elapsed << " s");
    MESSAGE("texture hot reload (detect + cook + swap): " << elapsed * 1000.0 << " ms");
    mount->release(asset::AnyHandle(ref.handle));
}

TEST_CASE("hot reload: a new material mounts, an edited one swaps in place") {
    const std::string root = reload_project("hr_material");
    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    session.tick_hot_reload(0.0, 0.0);
    std::filesystem::create_directories(root + "/assets/materials");
    put(root + "/assets/materials/paint.mcmat", material(0.5f));
    REQUIRE(session.tick_hot_reload(1.0, 0.0));
    CHECK(session.last_hot_reload().remounted);  // a new asset: fresh mount
    asset::AssetSystem* mount = session.project().mounted();
    const asset::AssetKey mat = key_of(session, "materials/paint.mcmat");
    REQUIRE(mat != asset::kInvalidAssetKey);
    auto h = mount->load_sync<asset::Material>(mat);
    REQUIRE(h);
    const u32 gen = mount->generation(mat);

    put(root + "/assets/materials/paint.mcmat", material(0.75f));
    touch_later(root + "/assets/materials/paint.mcmat");
    const f64 t0 = platform::now_seconds();
    REQUIRE(session.tick_hot_reload(2.0, 0.0));
    const f64 elapsed = platform::now_seconds() - t0;
    CHECK(session.last_hot_reload().reloaded == 1);
    CHECK_FALSE(session.last_hot_reload().remounted);
    CHECK(session.project().mounted() == mount);
    CHECK(mount->generation(mat) != gen);
    CHECK_MESSAGE(elapsed < 1.0, "FR-AST-090: material reload took " << elapsed << " s");
    MESSAGE("material hot reload (detect + cook + swap): " << elapsed * 1000.0 << " ms");
    mount->release(asset::AnyHandle(h.value()));
}

TEST_CASE("hot reload: a scene changed on disk is re-read unless the document has unsaved edits") {
    const std::string root = reload_project("hr_scene");
    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    REQUIRE(session.open_scene("scenes/demo.mcscene"));
    session.tick_hot_reload(0.0, 0.0);
    const usize before = session.scene().doc().size();

    // Another tool (an MCP client, a text editor) duplicates the last entity line in the file.
    const std::string path = root + "/assets/scenes/demo.mcscene";
    session.scene().selection().set(session.scene().doc().at(0).id);
    EditorSession other;
    REQUIRE(other.open_project(StringView(root.data(), root.size())));
    REQUIRE(other.open_scene("scenes/demo.mcscene"));
    other.scene().create_entity("FromOutside");
    REQUIRE(other.save_scene());
    touch_later(path);
    const f64 t0 = platform::now_seconds();
    REQUIRE(session.tick_hot_reload(1.0, 0.0));
    const f64 elapsed = platform::now_seconds() - t0;
    CHECK(session.last_hot_reload().scene_reloaded);
    CHECK(session.scene().doc().size() == before + 1);
    CHECK_FALSE(session.scene().dirty());
    CHECK(session.scene().selection().anchor() == session.scene().doc().at(0).id);
    CHECK_MESSAGE(elapsed < 1.0, "FR-AST-090: scene reload took " << elapsed << " s");
    MESSAGE("scene hot reload (detect + cook + re-read): " << elapsed * 1000.0 << " ms");

    // With unsaved edits the document wins; saving later overwrites the file.
    session.scene().create_entity("Mine");
    other.scene().create_entity("Second");
    REQUIRE(other.save_scene());
    touch_later(path);
    REQUIRE(session.tick_hot_reload(2.0, 0.0));
    CHECK_FALSE(session.last_hot_reload().scene_reloaded);
    CHECK(session.scene().dirty());
    CHECK(session.scene().doc().size() == before + 2);

    // The editor's own save is not a foreign change.
    REQUIRE(session.save_scene());
    REQUIRE(session.tick_hot_reload(3.0, 0.0));
    CHECK_FALSE(session.last_hot_reload().scene_reloaded);
}

TEST_CASE("hot reload: a broken texture keeps the previous output; play keeps running across a swap") {
    const std::string root = reload_project("hr_broken");
    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    REQUIRE(session.open_scene("scenes/demo.mcscene"));
    session.tick_hot_reload(0.0, 0.0);
    asset::AssetSystem* mount = session.project().mounted();
    const asset::AssetKey ball = key_of(session, "textures/ball.png");
    auto held = mount->load_sync<asset::Texture>(ball);  // resident: a swap bumps its generation
    REQUIRE(held);
    const u32 gen = mount->generation(ball);

    put(root + "/assets/textures/ball.png", "this is not a png");
    REQUIRE(session.tick_hot_reload(1.0, 0.0));
    CHECK(session.last_hot_reload().failed >= 1);
    CHECK(session.project().mounted() == mount);
    CHECK(mount->generation(ball) == gen);  // the old texture keeps serving

    REQUIRE(session.enter_play());
    session.play().advance(1.0 / 60.0);
    std::filesystem::copy_file(root + "/assets/textures/crate.png", root + "/assets/textures/ball.png",
                               std::filesystem::copy_options::overwrite_existing);
    touch_later(root + "/assets/textures/ball.png");
    REQUIRE(session.tick_hot_reload(2.0, 0.0));
    CHECK(session.last_hot_reload().reloaded >= 1);
    CHECK(session.play().active());  // a swap, not a remount: the play world lives on
    CHECK(session.play().advance(1.0 / 60.0) == 1);
    CHECK(mount->generation(ball) != gen);
    session.stop_play();
    mount->release(asset::AnyHandle(held.value()));
}
