// M-ASSETLOOK 3: the material form. The file text round trips through the form with every member the form does not
// own kept in place, defaults are not added to a file that lacks them, a save is cooked before it is written (a
// missing texture refuses it and leaves the file alone), and a good save is read back by the cook.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "cook_test_util.h"
#include "mce/cook/fs.h"
#include "mce/cook/json.h"
#include "mce/editor/asset_form.h"

using namespace mce;
using namespace mce::editor;

namespace {

constexpr const char* kGold = R"({
  "format": "mce-material",
  "version": 1,
  "guid": "5ce4e000-0000-4000-8000-0000000000c1",
  "name": "gold",
  "base_color": [1, 0.7, 0.1, 1],
  "metallic": 1,
  "sheen_color": [0.1, 0.1, 0.1],
  "roughness": 0.25
}
)";

}  // namespace

TEST_CASE("MaterialForm: text to form and back keeps unknown members in order and adds no defaults") {
    MaterialForm f;
    cook::String err;
    REQUIRE(material_form_from_text(kGold, f, err));
    CHECK(f.base_color[1] == doctest::Approx(0.7f));
    CHECK(f.metallic == 1.0f);
    CHECK(f.roughness == doctest::Approx(0.25f));
    CHECK(f.alpha_mode == 0);
    CHECK(f.texture[0].empty());

    f.roughness = 0.5f;
    f.double_sided = true;
    f.alpha_mode = 2;
    cook::String out;
    REQUIRE(material_form_to_text(kGold, f, out, err));
    cook::JsonDoc doc;
    cook::JsonError je;
    REQUIRE(doc.parse(out.view(), je));
    const cook::JsonValue root = doc.root();
    CHECK(root.get("roughness").as_number() == doctest::Approx(0.5));
    CHECK(root.get("double_sided").as_bool());
    CHECK(root.get("alpha_mode").as_string() == "blend");
    CHECK(root.get("name").as_string() == "gold");  // kept
    CHECK(root.get("sheen_color").size() == 3);      // kept
    CHECK_FALSE(root.get("unlit").valid());          // a default the file never had is not added
    CHECK_FALSE(root.get("textures").valid());
    CHECK_FALSE(root.get("ior").valid());
    // The members keep the file's order: format, version, guid, name, base_color, metallic, sheen_color, roughness, then the new ones.
    CHECK(root.key_at(3) == "name");
    CHECK(root.key_at(6) == "sheen_color");
    CHECK(root.key_at(7) == "roughness");
    CHECK(root.key_at(8) == "alpha_mode");
    CHECK(root.key_at(9) == "double_sided");

    MaterialForm again;
    REQUIRE(material_form_from_text(out.view(), again, err));
    CHECK(again.roughness == doctest::Approx(0.5f));
    CHECK(again.alpha_mode == 2);
    CHECK(again.double_sided);

    cook::String bad;
    CHECK_FALSE(material_form_from_text("[1, 2]", f, bad));
    CHECK_FALSE(material_form_from_text(R"({"format": "mce-scene"})", f, bad));
}

TEST_CASE("save_material_form: the cook checks the text first; a refused save leaves the file alone") {
    const std::string root = cook_test::fresh_dir("asset_form");
    std::filesystem::create_directories(root + "/assets/mat");
    std::ofstream(root + "/assets/mat/gold.mcmat") << kGold;
    EditorProject project;
    REQUIRE(project.open(root));
    const ProjectAsset* found = nullptr;
    for (const ProjectAsset& a : project.assets())
        if (a.path.view() == "mat/gold.mcmat") found = &a;
    REQUIRE(found);
    const ProjectAsset asset = *found;
    MaterialForm form;
    cook::String err;
    REQUIRE(load_material_form(project, asset, form, err));

    form.roughness = 0.75f;
    form.clearcoat = 0.5f;
    REQUIRE(save_material_form(project, asset, form, err));
    MaterialForm back;
    REQUIRE(load_material_form(project, asset, back, err));
    CHECK(back.roughness == doctest::Approx(0.75f));
    CHECK(back.clearcoat == doctest::Approx(0.5f));

    cook::String before;
    REQUIRE(cook::fs::read_text(project.asset_path(asset.path.view()).view(), before));
    form.texture[0] = cook::String("guid:5ce4e000-0000-4000-8000-00000000dead");  // no such texture
    err = cook::String();
    CHECK_FALSE(save_material_form(project, asset, form, err));
    CHECK(err.view().find("nothing was written") != StringView::npos);
    cook::String after;
    REQUIRE(cook::fs::read_text(project.asset_path(asset.path.view()).view(), after));
    CHECK(before.view() == after.view());

    // The swatch of the form is the scalar look.
    CHECK(form.swatch().metallic == 1.0f);
    CHECK(form.swatch().roughness == doctest::Approx(0.75f));
}

namespace {

constexpr const char* kEmbers = R"({
  "format": "mce-particles",
  "version": 1,
  "guid": "5ce4e000-0000-4000-8000-0000000000c2",
  "name": "embers",
  "rate": 30,
  "lifetime": [1.2, 2.2],
  "direction": 90,
  "blend": "additive"
}
)";

}  // namespace

TEST_CASE("ParticleForm: round trip keeps unknown members, converts nothing, adds no defaults") {
    ParticleForm f;
    cook::String err;
    REQUIRE(particle_form_from_text(kEmbers, f, err));
    CHECK(f.rate == 30.0f);
    CHECK(f.lifetime[1] == doctest::Approx(2.2f));
    CHECK(f.additive);
    CHECK(f.direction == 90.0f);
    CHECK(f.swatch().direction == doctest::Approx(1.5707963f));  // degrees in the file, radians in the swatch

    f.rate = 45;
    f.spread = 25;
    f.burst = 8;
    f.texture = cook::String("guid:5ce4e000-0000-4000-8000-00000000dead");
    cook::String out;
    REQUIRE(particle_form_to_text(kEmbers, f, out, err));
    cook::JsonDoc doc;
    cook::JsonError je;
    REQUIRE(doc.parse(out.view(), je));
    const cook::JsonValue root = doc.root();
    CHECK(root.get("name").as_string() == "embers");
    CHECK(root.get("rate").as_number() == 45.0);
    CHECK(root.get("burst").as_number() == 8.0);
    CHECK(root.get("blend").as_string() == "additive");
    CHECK_FALSE(root.get("drag").valid());
    CHECK_FALSE(root.get("max_particles").valid());
    CHECK(root.key_at(3) == "name");
    CHECK(root.key_at(4) == "rate");

    ParticleForm again;
    REQUIRE(particle_form_from_text(out.view(), again, err));
    CHECK(again.spread == 25.0f);
    CHECK(again.burst == 8);
    CHECK_FALSE(particle_form_from_text(R"({"format": "mce-material"})", f, err));
}

TEST_CASE("save_particle_form: a refused cook writes nothing, a good save reads back") {
    const std::string root = cook_test::fresh_dir("asset_form_part");
    std::filesystem::create_directories(root + "/assets/fx");
    std::ofstream(root + "/assets/fx/embers.mcpart") << kEmbers;
    EditorProject project;
    REQUIRE(project.open(root));
    const ProjectAsset* found = nullptr;
    for (const ProjectAsset& a : project.assets())
        if (a.path.view() == "fx/embers.mcpart") found = &a;
    REQUIRE(found);
    const ProjectAsset asset = *found;
    ParticleForm form;
    cook::String err;
    REQUIRE(load_particle_form(project, asset, form, err));
    form.drag = 0.6f;
    REQUIRE(save_particle_form(project, asset, form, err));
    ParticleForm back;
    REQUIRE(load_particle_form(project, asset, back, err));
    CHECK(back.drag == doctest::Approx(0.6f));

    cook::String before;
    REQUIRE(cook::fs::read_text(project.asset_path(asset.path.view()).view(), before));
    form.lifetime[0] = 5;  // min > max: the cook refuses
    err = cook::String();
    CHECK_FALSE(save_particle_form(project, asset, form, err));
    CHECK(err.view().find("nothing was written") != StringView::npos);
    cook::String after;
    REQUIRE(cook::fs::read_text(project.asset_path(asset.path.view()).view(), after));
    CHECK(before.view() == after.view());
}
