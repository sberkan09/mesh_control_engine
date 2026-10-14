// Sprite sheets (.mcsheet -> CHNK_SHEET) and AnimatedSprite (M-DATA-REACH 5).

#include <doctest/doctest.h>

#include <string>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/transform.h"
#include "mce/hash.h"
#include "mce/kit/components.h"
#include "mce/kit/sprite_anim.h"
#include "mce/scene/scene.h"

using namespace mce;
using namespace cook_test;
using kit::AnimatedSprite;
using kit::Sprite;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

std::string diag_text(const cook::CookReport& r) {
    std::string s;
    for (const cook::Diagnostic& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += std::string(line.view().data(), line.view().size()) + "\n";
    }
    return s;
}

constexpr const char* kSheet = R"({ "format": "mce-spritesheet", "version": 1,
    "guid": "9a270000-0000-4000-8000-000000000031", "name": "hero",
    "texture": "guid:9a270000-0000-4000-8000-000000000030",
    "grid": { "columns": 4, "rows": 2, "count": 6 },
    "image_size": [64, 32],
    "regions": [ { "name": "hurt", "rect": [48, 16, 16, 16] } ],
    "clips": [
        { "name": "run", "frames": { "from": 0, "to": 3 }, "fps": 10, "events": [ { "frame": 2, "name": "step" } ] },
        { "name": "die", "frames": [4, "hurt"], "fps": 5, "mode": "Once" } ] })";

}  // namespace

TEST_CASE("sprite sheet: .mcsheet cooks regions, clips and events; AnimatedSprite plays them on the Sprite") {
    const std::string root = fresh_dir("sheet_cook");
    std::vector<u8> px(64 * 32 * 4, 255);
    write_file(root + "/assets/hero.png", make_png(64, 32, px));
    write_file(root + "/assets/hero.png.mceasset", sidecar("9a270000-0000-4000-8000-000000000030", "texture"));
    write_file(root + "/assets/hero.mcsheet", kSheet);
    write_file(root + "/assets/level.mcscene", R"({ "format": "mce-scene", "version": 1,
        "guid": "9a270000-0000-4000-8000-000000000032", "name": "level", "entities": [
        { "name": "Hero", "components": {
            "LocalTransform": { "position": [0, 0, 0] },
            "Sprite": { "size": [1, 1] },
            "AnimatedSprite": { "sheet": "guid:9a270000-0000-4000-8000-000000000031", "clip": "run" } } } ] })");
    World w;
    kit::install_sprite_animation(w, nullptr);
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(diag_text(r));
    REQUIRE(r.ok());

    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    const asset::AssetKey key = assets.find(asset::parse_guid("9a270000-0000-4000-8000-000000000031").value());
    REQUIRE(key != asset::kInvalidAssetKey);
    auto h = assets.load_sync<asset::SpriteSheet>(key);
    REQUIRE(h);
    auto view = asset::SpriteSheetView::read(assets.resolve(h.value()));
    REQUIRE(view);
    const asset::SpriteSheetView& v = view.value();
    REQUIRE(v.regions.size() == 7);  // six grid cells, then "hurt"
    CHECK(v.regions[5].u0 == doctest::Approx(0.25f));  // cell 5: column 1 of the bottom row
    CHECK(v.regions[5].v0 == doctest::Approx(0.5f));
    CHECK(v.regions[6].u0 == doctest::Approx(0.75f));
    CHECK(v.regions[6].v1 == doctest::Approx(1.0f));
    REQUIRE(v.clips.size() == 2);
    const asset::SpriteSheetClip* die = v.find_clip("die");
    REQUIRE(die != nullptr);
    CHECK(die->frame_count == 2);
    CHECK(v.frames[die->first_frame + 1] == 6);
    CHECK(die->mode == 2);
    CHECK(v.find_clip("") == &v.clips[0]);
    REQUIRE(v.events.size() == 1);
    CHECK(v.events[0].id == static_cast<u32>(fnv1a64(StringView("step"))));
    CHECK(v.header->texture_guid() == asset::parse_guid("9a270000-0000-4000-8000-000000000030").value());
    assets.release(asset::AnyHandle(h.value()));

    kit::install_sprite_animation(w, &assets);
    Array<Entity> spawned;
    REQUIRE(scene::load(w, assets, asset::parse_guid("9a270000-0000-4000-8000-000000000032").value(), &spawned));
    REQUIRE(spawned.size() == 1);
    const Entity e = spawned[0];
    u64 t = 0;
    w.tick(kDt, t++);
    CHECK(w.try_get<Sprite>(e)->texture.guid == asset::parse_guid("9a270000-0000-4000-8000-000000000030").value());
    CHECK(w.try_get<Sprite>(e)->region.z == doctest::Approx(0.25f));  // frame 0 = cell 0
    CHECK(w.try_get<AnimatedSprite>(e)->playing_clip.view() == StringView("run"));

    // 10 fps: frame 2 (the "step" event) is entered after 0.2 s.
    for (u32 i = 0; i < 12; ++i) w.tick(kDt, t++);
    CHECK(w.try_get<AnimatedSprite>(e)->frame == 2);
    CHECK(w.try_get<AnimatedSprite>(e)->events_fired == 1);
    CHECK(w.try_get<AnimatedSprite>(e)->last_event.view() == StringView("step"));
    CHECK(w.try_get<Sprite>(e)->region.x == doctest::Approx(0.5f));
    for (u32 i = 0; i < 24; ++i) w.tick(kDt, t++);  // loops: frame 2 again after 0.6 s
    CHECK(w.try_get<AnimatedSprite>(e)->events_fired == 2);

    // Switching clip starts it from its first frame; a Once clip stops on its last.
    w.try_get_mut<AnimatedSprite>(e)->clip = "die";
    w.tick(kDt, t++);
    CHECK(w.try_get<AnimatedSprite>(e)->frame == 0);
    CHECK(w.try_get<Sprite>(e)->region.y == doctest::Approx(0.5f));  // cell 4: bottom row
    for (u32 i = 0; i < 60; ++i) w.tick(kDt, t++);
    CHECK(w.try_get<AnimatedSprite>(e)->finished);
    CHECK(w.try_get<AnimatedSprite>(e)->frame == 1);
    CHECK(w.try_get<Sprite>(e)->region.x == doctest::Approx(0.75f));  // "hurt"
    w.try_get_mut<AnimatedSprite>(e)->restart = true;
    w.tick(kDt, t++);
    CHECK_FALSE(w.try_get<AnimatedSprite>(e)->finished);
    CHECK(w.try_get<AnimatedSprite>(e)->frame == 0);
}

TEST_CASE("sprite sheet: .mcsheet errors name the field") {
    cook::CookService svc;
    auto errors = [&](const char* dir, const char* json) {
        const std::string root = fresh_dir(dir);
        write_file(root + "/assets/bad.mcsheet", json);
        const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
        CHECK_FALSE(r.ok());
        return diag_text(r);
    };
    CHECK(errors("sheet_e1", R"({ "format": "mce-spritesheet", "version": 1, "guid": "9a270000-0000-4000-8000-000000000041",
        "regions": [ { "rect": [0, 0, 8, 8] } ] })").find("image_size") != std::string::npos);
    CHECK(errors("sheet_e2", R"({ "format": "mce-spritesheet", "version": 1, "guid": "9a270000-0000-4000-8000-000000000042",
        "grid": { "columns": 2, "rows": 1 }, "clips": [ { "name": "a", "frames": [0, 2] } ] })").find("region index") != std::string::npos);
    CHECK(errors("sheet_e3", R"({ "format": "mce-spritesheet", "version": 1, "guid": "9a270000-0000-4000-8000-000000000043",
        "grid": { "columns": 2, "rows": 1 }, "clips": [ { "name": "a", "frames": [0], "mode": "Bounce" } ] })").find("mode") != std::string::npos);
    CHECK(errors("sheet_e4", R"({ "format": "mce-spritesheet", "version": 1, "guid": "9a270000-0000-4000-8000-000000000044",
        "grid": { "columns": 2, "rows": 1 }, "clips": [ { "name": "a", "frames": [0, 1], "events": [ { "frame": 2, "name": "x" } ] } ] })")
              .find("within the clip") != std::string::npos);
}
