// mce_scene: cooked scenes become entities through reflection alone (FR-ENT-060, FR-DX-020).

#include <doctest/doctest.h>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/transform.h"
#include "mce/kit/sprite_extract.h"
#include "mce/scene/entity_names.h"
#include "mce/scene/scene.h"

using namespace mce;
using namespace cook_test;

namespace scenetest {

struct SnFollow {
    Entity target;
    f32 distance = 2.0f;
    u32 pad_ = 0;
};
MCE_COMPONENT(SnFollow, target, distance)

}  // namespace scenetest

namespace {

constexpr const char* kTexGuid = "5ce4e000-1111-4000-8000-000000000001";
constexpr const char* kSceneGuid = "5ce4e000-1111-4000-8000-000000000002";

std::string make_scene_project(const char* name, const std::string& entities) {
    const std::string root = fresh_dir(name);
    std::vector<u8> px(4 * 4 * 4, 200);
    write_file(root + "/assets/tex.png", make_png(4, 4, px));
    write_file(root + "/assets/tex.png.mceasset", std::string("{ \"guid\": \"") + kTexGuid +
                                                      "\", \"importer\": \"texture\", \"importer_version\": 1, "
                                                      "\"settings\": { \"srgb\": true, \"mips\": false } }");
    write_file(root + "/assets/level.mcscene", std::string("{ \"format\": \"mce-scene\", \"version\": 1, \"guid\": \"") +
                                                   kSceneGuid + "\", \"name\": \"level\", \"entities\": [" + entities + "] }");
    return root;
}

std::string diag_text(const cook::CookReport& r) {
    std::string s;
    for (const cook::Diagnostic& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += std::string(line.view().data(), line.view().size()) + "\n";
    }
    return s;
}

}  // namespace

TEST_CASE("scene: cook -> mount -> instantiate stamps components and entity references") {
    const std::string root = make_scene_project("scene_load",
        R"({ "name": "Target", "components": { "LocalTransform": { "position": [1, 2, 0] } } },
           { "name": "Follower", "components": {
               "LocalTransform": { "position": [-3, 0.5, 0], "scale": [2, 2, 1] },
               "SnFollow": { "target": "$0", "distance": 4.5 },
               "Sprite": { "texture": "guid:5ce4e000-1111-4000-8000-000000000001", "size": [2, 3], "layer": 7 } } },
           { "name": "Camera", "components": {
               "LocalTransform": { "position": [5, 6, 0] },
               "Camera2DComponent": { "zoom": 2, "pixels_per_meter": 32 } } },
           { "name": "Orphan", "components": { "SnFollow": { "target": null } } })");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(diag_text(r));
    REQUIRE(r.ok());

    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));

    World w;
    Array<Entity> spawned;
    REQUIRE(scene::load(w, assets, asset::parse_guid(kSceneGuid).value(), &spawned));
    REQUIRE(spawned.size() == 4);
    CHECK(w.entity_count() == 4);

    const LocalTransform* t0 = w.try_get<LocalTransform>(spawned[0]);
    REQUIRE(t0);
    CHECK(t0->position.x == 1.0f);
    CHECK(t0->position.y == 2.0f);
    CHECK(t0->rotation == math::quat::identity());  // omitted fields keep C++ defaults
    CHECK(t0->scale.x == 1.0f);

    const scenetest::SnFollow* f = w.try_get<scenetest::SnFollow>(spawned[1]);
    REQUIRE(f);
    CHECK(f->target == spawned[0]);  // "$0" -> the spawned entity, not the index
    CHECK(f->distance == 4.5f);
    const scenetest::SnFollow* orphan = w.try_get<scenetest::SnFollow>(spawned[3]);
    REQUIRE(orphan);
    CHECK(orphan->target.is_null());
    CHECK_FALSE(w.has<LocalTransform>(spawned[3]));

    const kit::Sprite* s = w.try_get<kit::Sprite>(spawned[1]);
    REQUIRE(s);
    CHECK(s->texture.guid == asset::parse_guid(kTexGuid).value());
    CHECK(s->size.x == 2.0f);
    CHECK(s->layer == 7);
    CHECK(s->tint == math::Color::white());

    render::Camera2D cam;
    f32 ppm = 0;
    REQUIRE(kit::primary_camera(w, cam, ppm));
    CHECK(cam.position.x == 5.0f);
    CHECK(cam.zoom == 2.0f);
    CHECK(ppm == 32.0f);

    // Instantiating twice spawns a second, independent copy.
    Array<Entity> again;
    REQUIRE(scene::load(w, assets, asset::parse_guid(kSceneGuid).value(), &again));
    CHECK(w.entity_count() == 8);
    CHECK(w.try_get<scenetest::SnFollow>(again[1])->target == again[0]);
}

TEST_CASE("scene: a missing scene GUID is an error, never a crash") {
    asset::AssetSystem assets;
    World w;
    const Result<void> r = scene::load(w, assets, asset::parse_guid(kSceneGuid).value());
    CHECK_FALSE(r);
    CHECK(w.entity_count() == 0);
}

TEST_CASE("scene: components registered from reflection unify with typed registration") {
    World w;
    const ecs::ComponentId id = scene::component_for(w, refl::type_of<LocalTransform>());
    CHECK(id == w.component_id<LocalTransform>());
    CHECK(refl::type_of<LocalTransform>().type_hash == kTypeHash<LocalTransform>);
    CHECK(refl::type_of<kit::Sprite>().is_component());
}

TEST_CASE("scene: entities are found by name, names go with a scene change, and the hash does not see them (M-WORKFLOW 7)") {
    const std::string root = make_scene_project("scene_names",
        R"({ "name": "Target", "components": { "LocalTransform": { "position": [1, 2, 0] } } },
           { "name": "Camera", "components": { "LocalTransform": { "position": [5, 6, 0] } } },
           { "components": { "LocalTransform": { "position": [9, 9, 0] } } })");
    cook::CookService svc;
    REQUIRE(svc.cook(StringView(root.c_str()), cook::CookOptions{}).ok());
    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));

    World w;
    Array<Entity> spawned;
    REQUIRE(scene::load(w, assets, asset::parse_guid(kSceneGuid).value(), &spawned));
    CHECK(scene::find_entity(w, "Target") == spawned[0]);
    CHECK(scene::find_entity(w, "Camera") == spawned[1]);
    CHECK(scene::find_entity(w, "Nobody").is_null());
    CHECK(scene::find_entity(w, "").is_null());  // the unnamed entity is not found by ""

    // The table is a resource, not a component: dropping it leaves the state hash as it was.
    const u64 with_names = w.state_hash();
    w.set_resource(scene::EntityNames{});
    CHECK(w.state_hash() == with_names);

    // Instantiated again, the earlier copy keeps winning while it lives; once destroyed, the next does.
    REQUIRE(scene::load(w, assets, asset::parse_guid(kSceneGuid).value(), &spawned));
    const Entity second = scene::find_entity(w, "Target");
    CHECK_FALSE(second.is_null());
    w.destroy(second);
    CHECK(scene::find_entity(w, "Target").is_null());

    // A scene change clears the table, then the new scene fills it.
    scene::request_change(w, std::string("guid:").append(kSceneGuid));
    Array<Entity> after;
    REQUIRE(scene::apply_change(w, assets, &after).value());
    CHECK(scene::find_entity(w, "Camera") == after[1]);
    CHECK(w.alive(after[1]));
}
